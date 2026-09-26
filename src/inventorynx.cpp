#include "pch.h"
#include "hook.h"
#include "ztl/ztl.h"        // IWzCanvas / IWzFont / Ztl_bstr_t / Ztl_variant_t
#include "wvs/packet.h"     // CInPacket (the NX packet)
#include "cashshopwnd.h"     // CashShopWnd_SetNxCredit (the Cash Shop window shows the same value)

#include <cstdio>

// Inventory NX row. UI/UIWindow.img Item/backgrnd and Item/FullBackgrnd (Data) carry a second box
// under the meso box, 18px taller (289 -> 307, box top at y=285). This file grows the window to
// match and draws the character's NX Credit into that box, right-aligned like the meso value.
//
// The client never knows NX outside the cash shop, so the value comes from the server:
// SendOpcode.INVENTORY_CASH (0x3741), one long = NX Credit (CashShop.NX_CREDIT, up to
// GameConstants.MAX_NX_CREDIT = 9,999,999,999,999, the meso ceiling). The server sends
// it on every map entry and after every NX change.

namespace {

// IWzCanvas::DrawTextA — v83 VA 0x004277AD (ret 0x18). The meso value is drawn by the one call
// at 0x0081DD73 in CUIItem::Draw (v95 sym, v83 VA 0x0081DC20): x = 0x8A - width, y = 0x10A.
constexpr uintptr_t kAddr_DrawTextA      = 0x004277AD;
constexpr uintptr_t kSite_MesoDrawText   = 0x0081DD73;
typedef unsigned int(__thiscall* t_DrawTextA)(void* pCanvas, int nLeft, int nTop, void* pText,
                                              void* pFont, void* pVAlpha, void* pVTabOrg);
auto DrawTextA_real = reinterpret_cast<t_DrawTextA>(kAddr_DrawTextA);

// CWnd::CreateWnd height pushes (`push 0x121`) for the inventory window:
//   0x0081C512 in CUIItem::CUIItem (v83 VA 0x0081C414)
//   0x0081E5BC in sub_0081E541, the Full/Small toggle (flips +0x604, Destroy, re-create)
// Width is computed separately (0xAF, or 603 in full mode) and is unchanged.
constexpr uintptr_t kSite_CtorHeight   = 0x0081C512;
constexpr uintptr_t kSite_ToggleHeight = 0x0081E5BC;
constexpr unsigned int kStockHeight = 0x121;   // 289
constexpr unsigned int kNxHeight    = 0x133;   // 307: + one 18px band (rows 265..282 copied after 283)

constexpr int kValueRight = 0x8A;       // meso right edge (0x0081DCD8 mov esi, 0x8A)
constexpr int kNxRowDelta = 19;         // meso box top y=266, NX box top y=285

// TSingleton<CUIItem>::ms_pInstance — written with `this` by CUIItem::CUIItem at 0x0081C455; null
// while the inventory is closed. CWnd is the primary base, at +0.
constexpr uintptr_t kAddr_CUIItem_Instance = 0x00BED654;
// CWnd::InvalidateRect — v83 IDB sym ?InvalidateRect@CWnd@@QAEXPBUtagRECT@@@Z, VA 0x009E04C9
// (__thiscall, ret 4; a null rect invalidates the whole window).
auto kInvalidateRect = reinterpret_cast<void(__thiscall*)(void*, const RECT*)>(0x009E04C9);

bool g_bNxKnown = false;                // blank until the first packet (it arrives on field entry)
long long g_nNxCredit = 0;

// "1,234,567", as format_integer(…, bComma=1) prints mesos.
void FormatWithCommas(long long nValue, char* sOut, size_t uSize) {
    char sDigits[24];
    const bool bNeg = nValue < 0;
    const unsigned long long u = bNeg ? 0ull - static_cast<unsigned long long>(nValue)
                                      : static_cast<unsigned long long>(nValue);
    int n = std::snprintf(sDigits, sizeof(sDigits), "%llu", u);
    size_t o = 0;
    if (bNeg && o + 1 < uSize) sOut[o++] = '-';
    for (int i = 0; i < n && o + 1 < uSize; ++i) {
        if (i > 0 && (n - i) % 3 == 0 && o + 1 < uSize) sOut[o++] = ',';
        if (o + 1 < uSize) sOut[o++] = sDigits[i];
    }
    sOut[o] = '\0';
}

void DrawNx(void* pCanvas, int nTop, void* pFont, void* pVAlpha, void* pVTabOrg) {
    char sText[32];
    FormatWithCommas(g_nNxCredit, sText, sizeof(sText));
    try {
        IWzCanvas* pWzCanvas = reinterpret_cast<IWzCanvas*>(pCanvas);
        IWzFont* pWzFont = reinterpret_cast<IWzFont*>(pFont);
        const int nWidth = static_cast<int>(pWzFont->CalcTextWidth(Ztl_bstr_t(sText), Ztl_variant_t()));
        const int nLeft = kValueRight - nWidth;
        if (pVAlpha != nullptr && pVTabOrg != nullptr) {
            pWzCanvas->DrawTextA(nLeft, nTop, Ztl_bstr_t(sText), pWzFont,
                                 *reinterpret_cast<Ztl_variant_t*>(pVAlpha),
                                 *reinterpret_cast<Ztl_variant_t*>(pVTabOrg));
        } else {
            pWzCanvas->DrawTextA(nLeft, nTop, Ztl_bstr_t(sText), pWzFont,
                                 Ztl_variant_t(), Ztl_variant_t());
        }
    } catch (...) {
        // A missed NX value is cosmetic; never take the inventory down for it.
    }
}

// Replaces the meso `call DrawTextA` only. __fastcall so ECX carries the canvas and the six stack
// args are callee-cleaned, matching DrawTextA's `ret 0x18`. The real call consumes pText (a bstr
// passed by value); the canvas and font stay owned by CUIItem::Draw until after we return.
unsigned int __fastcall MesoDrawText_hook(void* pCanvas, void* /*edx*/, int nLeft, int nTop,
                                          void* pText, void* pFont, void* pVAlpha, void* pVTabOrg) {
    const unsigned int uWidth = DrawTextA_real(pCanvas, nLeft, nTop, pText, pFont, pVAlpha, pVTabOrg);
    if (g_bNxKnown) {
        DrawNx(pCanvas, nTop + kNxRowDelta, pFont, pVAlpha, pVTabOrg);
    }
    return uWidth;
}

bool IsPushImm32(uintptr_t uAddress, unsigned int uImm) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(uAddress);
    return p[0] == 0x68 && *reinterpret_cast<const unsigned int*>(uAddress + 1) == uImm;
}

bool IsCallTo(uintptr_t uAddress, uintptr_t uTarget) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(uAddress);
    const int nRel = *reinterpret_cast<const int*>(uAddress + 1);
    return p[0] == 0xE8 && uAddress + 5 + static_cast<uintptr_t>(nRel) == uTarget;
}

} // namespace

// SendOpcode.INVENTORY_CASH (0x3741). Routed from clientsocket.cpp, which only Peek2s, so the
// opcode is skipped here.
void InventoryNx_HandlePacket(CInPacket* pPacket) {
    pPacket->Decode<unsigned short>();
    g_nNxCredit = pPacket->Decode<long long>();   // writeLong since the NX uncap
    g_bNxKnown = true;
    CashShopWnd_SetNxCredit(g_nNxCredit);
    // CUIItem::Draw runs only when the window is invalidated, so an open inventory would keep the
    // old value until something else repainted it. ProcessPacket runs on the main thread.
    void* pInventory = *reinterpret_cast<void**>(kAddr_CUIItem_Instance);
    if (pInventory != nullptr) {
        kInvalidateRect(pInventory, nullptr);   // CWnd::InvalidateRect(nullptr) — whole window
    }
}

void AttachInventoryNxMod() {
    // All or nothing: a taller window without the text, or text below a 289px window, is worse
    // than stock. The art is already 307px, so a skipped patch shows up as a clipped bottom edge.
    if (!IsPushImm32(kSite_CtorHeight, kStockHeight) || !IsPushImm32(kSite_ToggleHeight, kStockHeight) ||
        !IsCallTo(kSite_MesoDrawText, kAddr_DrawTextA)) {
        ErrorMessage("Inventory NX: unexpected bytes at the CUIItem height/meso-draw sites - skipped.");
        return;
    }
    Patch4(kSite_CtorHeight + 1, kNxHeight);    // CUIItem::CUIItem — CreateWnd height 289 -> 307 for the NX row
    Patch4(kSite_ToggleHeight + 1, kNxHeight);  // sub_0081E541 (Full/Small toggle) — same re-create height
    PatchCall(kSite_MesoDrawText, reinterpret_cast<uintptr_t>(&MesoDrawText_hook)); // CUIItem::Draw — meso DrawTextA, then NX one row below
}
