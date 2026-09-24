#include "pch.h"
#include "hook.h"
#include "wvs/packet.h"
#include "ztl/ztl.h"
#include <climits>

// Meso balances widened to 64 bits, capped at kMaxMeso (server-side).
//
// Packet contract with the server — each balance is Decode4 -> Decode8:
//   wallet     GW_CharacterStat::DecodeMoney, GW_CharacterStat::DecodeChangeStat (0x40000)
//   storage    CTrunkDlg::SetGetItems (flag 2)
//   Fredrick   CStoreBankDlg set-up (flag 2)
//   merchant   CEntrustedShopDlg sold-item list (+0x184) and refresh (+0x784)
// Single transactions (trade offers, deposit/withdraw amounts, prices, drops) stay 32-bit.
//
// Same scheme as maxlevel.cpp EXP: the native 32-bit field receives min(value, INT_MAX), so every native
// comparison (can afford, deposit <= balance, quest demand) still holds for transactions up to INT_MAX. The
// real value is shadowed here, and each balance display swaps format_integer for a version that prints the
// shadow when the native value is the clamp.

namespace {

// CInPacket::Decode4 — v83 VA 0x00406629
constexpr uintptr_t kDecode4 = 0x00406629;
// format_integer(ZXString<char>&, long, int bComma) — v95 sym, v83 VA 0x00988690 (__cdecl)
constexpr uintptr_t kFormatInteger = 0x00988690;
// CTrunkDlg::DrawMoney(IWzCanvasPtr, long nMoney, long nY) — v95 sym, v83 VA 0x007C8064
constexpr uintptr_t kTrunkDrawMoney = 0x007C8064;
// CEntrustedShopDlg::GetMoney — v95 sym, v83 VA 0x0051819A; vtable slot +0x74 (CPersonalShopDlg: 0x006FC365)
constexpr uintptr_t kEntrustedGetMoney = 0x0051819A;
constexpr size_t kGetMoneySlot = 0x74 / sizeof(void*);

// CInPacket::DecodeBuffer — v83 VA 0x00432257
auto CInPacket__DecodeBuffer = reinterpret_cast<void(__thiscall*)(CInPacket*, void*, size_t)>(0x00432257);
auto format_integer = reinterpret_cast<void(__cdecl*)(ZXString<char>*, long, int)>(kFormatInteger);

long long g_nMeso = 0;          // wallet
long long g_nTrunkMeso = 0;     // storage
long long g_nStoreBankMeso = 0; // Fredrick
long long g_nMerchantMeso = 0;  // hired merchant, owner's sold-item dialog (+0x184)
long long g_nShopMeso = 0;      // hired merchant dialog (+0x784)

uintptr_t g_uTrunkDrawMoney = kTrunkDrawMoney;
int g_bTrunkDrawTrunk = 0;      // which balance the current CTrunkDlg::DrawMoney call shows
void* g_pShopDlg = nullptr;     // CPersonalShopDlg / CEntrustedShopDlg being drawn


bool BytesMatch(uintptr_t uAddress, const void* pExpected, size_t uSize) {
    if (memcmp(reinterpret_cast<const void*>(uAddress), pExpected, uSize) == 0) {
        return true;
    }
#ifdef _DEBUG
    ErrorMessage("[maxmeso] unexpected bytes at 0x%08X, patch skipped", uAddress);
#else
    LogMessage("[maxmeso] unexpected bytes at 0x%08X, patch skipped", uAddress);
#endif
    return false;
}

// Redirect a call only if it still targets uExpected.
void PatchCallExpect(uintptr_t uAddress, uintptr_t uExpected, const void* pDestination) {
    unsigned char abExpected[5] = { 0xE8 };
    unsigned int uRel = static_cast<unsigned int>(uExpected - uAddress - 5);
    memcpy(abExpected + 1, &uRel, sizeof(uRel));
    if (BytesMatch(uAddress, abExpected, sizeof(abExpected))) {
        PatchCall(uAddress, pDestination);
    }
}


long long DecodeMoney8(CInPacket* pPacket) {
    long long nMoney = 0;
    CInPacket__DecodeBuffer(pPacket, &nMoney, sizeof(nMoney));
    return (std::max)(nMoney, 0LL);
}

int Clamp(long long nMoney) {
    return static_cast<int>((std::min)(nMoney, static_cast<long long>(INT_MAX)));
}

// Each replaces "call CInPacket::Decode4" (ECX = CInPacket*) and returns the clamped value to the native store.
int __fastcall Wallet_Decode4To8(CInPacket* pPacket, void* _EDX) {
    g_nMeso = DecodeMoney8(pPacket);
    return Clamp(g_nMeso);
}

int __fastcall Trunk_Decode4To8(CInPacket* pPacket, void* _EDX) {
    g_nTrunkMeso = DecodeMoney8(pPacket);
    return Clamp(g_nTrunkMeso);
}

int __fastcall StoreBank_Decode4To8(CInPacket* pPacket, void* _EDX) {
    g_nStoreBankMeso = DecodeMoney8(pPacket);
    return Clamp(g_nStoreBankMeso);
}

int __fastcall Merchant_Decode4To8(CInPacket* pPacket, void* _EDX) {
    g_nMerchantMeso = DecodeMoney8(pPacket);
    return Clamp(g_nMerchantMeso);
}

int __fastcall Shop_Decode4To8(CInPacket* pPacket, void* _EDX) {
    g_nShopMeso = DecodeMoney8(pPacket);
    return Clamp(g_nShopMeso);
}


// format_integer with the shadow substituted when the native value is the clamp. Matches the native output:
// decimal digits, "," every three digits from the right when bComma.
void FormatMoney(ZXString<char>* sOut, long nValue, int bComma, long long nShadow) {
    if (nValue != INT_MAX || nShadow <= INT_MAX) {
        format_integer(sOut, nValue, bComma);
        return;
    }
    char sDigits[32];
    _i64toa_s(nShadow, sDigits, sizeof(sDigits), 10);
    char sBuffer[48];
    int nLength = static_cast<int>(strlen(sDigits));
    int n = 0;
    for (int i = 0; i < nLength; ++i) {
        if (bComma && i > 0 && (nLength - i) % 3 == 0) {
            sBuffer[n++] = ',';
        }
        sBuffer[n++] = sDigits[i];
    }
    sOut->Assign(sBuffer, n);
}

void __cdecl Format_Wallet(ZXString<char>* sOut, long nValue, int bComma) {
    FormatMoney(sOut, nValue, bComma, g_nMeso);
}

void __cdecl Format_StoreBank(ZXString<char>* sOut, long nValue, int bComma) {
    FormatMoney(sOut, nValue, bComma, g_nStoreBankMeso);
}

void __cdecl Format_Merchant(ZXString<char>* sOut, long nValue, int bComma) {
    FormatMoney(sOut, nValue, bComma, g_nMerchantMeso);
}

void __cdecl Format_Trunk(ZXString<char>* sOut, long nValue, int bComma) {
    FormatMoney(sOut, nValue, bComma, g_bTrunkDrawTrunk ? g_nTrunkMeso : g_nMeso);
}

// CPersonalShopDlg::Draw formats the virtual GetMoney(). For CEntrustedShopDlg, GetMoney returns +0x784 when
// [this+0xC8] == 0 and the wallet otherwise (0x0051819A); CPersonalShopDlg::GetMoney is always the wallet.
void __cdecl Format_ShopDlg(ZXString<char>* sOut, long nValue, int bComma) {
    long long nShadow = g_nMeso;
    if (g_pShopDlg) {
        uintptr_t* pVtbl = *reinterpret_cast<uintptr_t**>(g_pShopDlg);
        int bOwnMoney = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(g_pShopDlg) + 0xC8);
        if (pVtbl[kGetMoneySlot] == kEntrustedGetMoney && bOwnMoney == 0) {
            nShadow = g_nShopMeso;
        }
    }
    FormatMoney(sOut, nValue, bComma, nShadow);
}

// ESI = this at the format_integer call in CPersonalShopDlg::Draw (0x006FB9B3 mov esi, ecx).
void __declspec(naked) Format_ShopDlg_Thunk() {
    __asm {
        mov     g_pShopDlg, esi
        jmp     Format_ShopDlg
    }
}

// Replace the two "call CTrunkDlg::DrawMoney" in CTrunkDlg::Draw; the stack is left untouched.
void __declspec(naked) TrunkDrawMoney_Trunk() {
    __asm {
        mov     g_bTrunkDrawTrunk, 1
        jmp     dword ptr [g_uTrunkDrawMoney]
    }
}

void __declspec(naked) TrunkDrawMoney_Wallet() {
    __asm {
        mov     g_bTrunkDrawTrunk, 0
        jmp     dword ptr [g_uTrunkDrawMoney]
    }
}

} // namespace


void AttachMaxMesoMod() {
    // Decode4 -> Decode8: balances the server now writes as long
    PatchCallExpect(0x004E2CFC, kDecode4, &Wallet_Decode4To8);    // GW_CharacterStat::DecodeMoney (v83 0x004E2CF5) — nMoney
    PatchCallExpect(0x004E31F6, kDecode4, &Wallet_Decode4To8);    // GW_CharacterStat::DecodeChangeStat — MONEY (0x40000)
    PatchCallExpect(0x007C5E40, kDecode4, &Trunk_Decode4To8);     // CTrunkDlg::SetGetItems — flag 2, +0xFC
    PatchCallExpect(0x00799DE1, kDecode4, &StoreBank_Decode4To8); // CStoreBankDlg set-up (v83 0x00799DA1) — flag 2, +0xB8
    PatchCallExpect(0x00518FBC, kDecode4, &Merchant_Decode4To8);  // CEntrustedShopDlg sold-item list (v83 0x00518EFD) — +0x184
    PatchCallExpect(0x00518859, kDecode4, &Shop_Decode4To8);      // CEntrustedShopDlg refresh (v83 0x00518852) — +0x784

    // format_integer -> 64-bit shadow at each balance display
    PatchCallExpect(0x0081DC84, kFormatInteger, &Format_Wallet);         // CUIItem::Draw — inventory meso
    PatchCallExpect(0x00755E97, kFormatInteger, &Format_Wallet);         // CShopDlg::DrawMoney — NPC shop meso
    PatchCallExpect(0x0079B058, kFormatInteger, &Format_StoreBank);      // CStoreBankDlg::DrawMoney (v83 0x0079B034) — Fredrick
    PatchCallExpect(0x006FFBCC, kFormatInteger, &Format_Merchant);       // CPersonalShopDlg sold-item dialog — +0x184
    PatchCallExpect(0x006FB9D3, kFormatInteger, &Format_ShopDlg_Thunk);  // CPersonalShopDlg::Draw — GetMoney()
    PatchCallExpect(0x007C8088, kFormatInteger, &Format_Trunk);          // CTrunkDlg::DrawMoney — trunk or wallet
    PatchCallExpect(0x007C77A5, kTrunkDrawMoney, &TrunkDrawMoney_Trunk);  // CTrunkDlg::Draw — DrawMoney([esi+0xFC], 0xBE)
    PatchCallExpect(0x007C77C8, kTrunkDrawMoney, &TrunkDrawMoney_Wallet); // CTrunkDlg::Draw — DrawMoney(wallet, 0x1A2)
}
