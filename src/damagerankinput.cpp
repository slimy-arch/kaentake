#include "pch.h"
#include "hook.h"
#include "uiDamageRank.h"
#include "wvs/field.h"

// DamageRank input: Key Config function key plus field mouse forwarding. The window is a bare Gr2D layer,
// not a CWnd, so it only sees input the field would otherwise get; drag/wheel fallbacks live in bypass.cpp.
//
// The toggle is a Key Config shortcut: FUNCKEY_MAPPED type 4 (UI menu), id 55, icon
// UI/UIWindow.img/KeyConfig/icon/55. The icon ships in Custom.wz at the same path, and resman.cpp's
// serialize hook merges it into the stock KeyConfig/icon property, so the client reads it like any other
// icon. The stock palette holds 40 fixed slots (type 4 ids 0-27, type 5 ids
// 50-54, type 6 ids 100-106); id 55 goes into slot 40, whose position (145,335) already exists in the
// palette position table at 0x00BE27E0. The server stores any non-skill binding as-is, and its default
// keymaps put this on F12 (scan code 88).

namespace {

constexpr int kDamageRankFuncKeyType = 4;  // FUNCKEY_MAPPED::nType — UI menu shortcut
constexpr int kDamageRankFuncKeyId = 55;
constexpr int kDamageRankPaletteSlot = 40; // first slot past the stock 40
constexpr int kFuncKeyCount = 89;          // CFuncKeyMappedMan entries, indexed by set-1 scan code

// CUIKeyConfig palette record: this + 0x9DC + 12 * slot = { char nType; int nID (unaligned); int bInUse @+8 }.
// The ctor zeroes 0x288 bytes (54 records), so slot 40 is inside the object.
constexpr uintptr_t kPaletteBase = 0x9DC;
constexpr uintptr_t kPaletteStride = 12;

// CUIKeyConfig::ResetPaletteItems — v95 sym, v83 VA 0x00836619 (plain ret; loop bounded by cmp edi,28h)
auto CUIKeyConfig__ResetPaletteItems = reinterpret_cast<void(__thiscall*)(void*)>(0x00836619);

// CUIKeyConfig::GetIdxFromPaletteSlot — v95 sym, v83 VA 0x00836C82 (ret 4)
auto CUIKeyConfig__GetIdxFromPaletteSlot = reinterpret_cast<int(__thiscall*)(void*, int)>(0x00836C82);

// CUIKeyConfig::GetPaletteSlotFromIdx — v95 sym, v83 VA 0x00836C5F (ret 8)
auto CUIKeyConfig__GetPaletteSlotFromIdx = reinterpret_cast<int(__thiscall*)(void*, int, int)>(0x00836C5F);

// CWvsContext::ProcessBasicUIKey — v95 sym, v83 VA 0x00A07431 (ret 8); reached from CField::OnKey and
// CUIWnd::OnKey, so type-4 keys work with another window focused. Rejects ids >= 0x1C at 0x00A0755E.
auto CWvsContext__ProcessBasicUIKey = reinterpret_cast<int(__thiscall*)(void*, unsigned int, int)>(0x00A07431);

// CField::OnMouseButton — v95 sym, v83 VA 0x005299DE (ret 0x10)
auto CField__OnMouseButton = reinterpret_cast<void(__thiscall*)(CField*, unsigned int, unsigned int, int, int)>(0x005299DE);

// CField::OnMouseMove — v95 sym, v83 VA 0x00529A09 (ret 8)
auto CField__OnMouseMove = reinterpret_cast<int(__thiscall*)(CField*, int, int)>(0x00529A09);

// CWnd::OnMouseWheel — v95 sym, v83 VA 0x009E02AE (ret 0xC)
auto CWnd__OnMouseWheel = reinterpret_cast<int(__thiscall*)(void*, int, int, int)>(0x009E02AE);

bool GetGameCursorPoint(POINT& pt) {
    void* pInputSystem = *reinterpret_cast<void**>(0x00BEC33C); // TSingleton<CInputSystem>::ms_pInstance
    if (!pInputSystem) {
        return false;
    }
    pt = {};
    // CInputSystem::GetCursorPos — v95 sym, v83 VA 0x0059A388 (ret 4)
    reinterpret_cast<int(__thiscall*)(void*, POINT*)>(0x0059A388)(pInputSystem, &pt);
    return true;
}

void __fastcall CField__OnMouseButton_hook(CField* pThis, void* _EDX, unsigned int msg, unsigned int wParam, int rx, int ry) {
    auto& ui = CUIDamageRank::GetInstance();
    if (msg == WM_LBUTTONDOWN) {
        if (ui.HandleMouseLButtonDown(rx, ry)) {
            return;
        }
    } else if (msg == WM_LBUTTONUP) {
        if (ui.HandleMouseLButtonUp(rx, ry)) {
            return;
        }
    }
    CField__OnMouseButton(pThis, msg, wParam, rx, ry);
}

int __fastcall CField__OnMouseMove_hook(CField* pThis, void* _EDX, int rx, int ry) {
    if (CUIDamageRank::GetInstance().HandleMouseMove(rx, ry)) {
        return 1;
    }
    return CField__OnMouseMove(pThis, rx, ry);
}

int __fastcall CWnd__OnMouseWheel_hook(void* pThis, void* _EDX, int rx, int ry, int nWheel) {
    POINT pt;
    if (GetGameCursorPoint(pt) && CUIDamageRank::GetInstance().HandleMouseWheel(pt.x, pt.y, nWheel)) {
        return 1;
    }
    return CWnd__OnMouseWheel(pThis, rx, ry, nWheel);
}

void ToggleDamageRankFromKey() {
    auto& ui = CUIDamageRank::GetInstance();
    const bool bWasVisible = ui.IsVisible();
    CUIDamageRank::ToggleByHotkey();
    if (!bWasVisible && ui.IsVisible()) {
        CUIDamageRank::PlayUISound(L"MenuUp");
    }
}

void __fastcall CUIKeyConfig__ResetPaletteItems_hook(void* pThis, void* _EDX) {
    CUIKeyConfig__ResetPaletteItems(pThis);
    auto pRecord = reinterpret_cast<unsigned char*>(pThis) + kPaletteBase + kPaletteStride * kDamageRankPaletteSlot;
    const int nId = kDamageRankFuncKeyId;
    pRecord[0] = static_cast<unsigned char>(kDamageRankFuncKeyType);
    memcpy(pRecord + 1, &nId, sizeof(nId));
    *reinterpret_cast<int*>(pRecord + 8) = 0; // not in use: shown in the palette
}

int __fastcall CUIKeyConfig__GetIdxFromPaletteSlot_hook(void* pThis, void* _EDX, int nSlot) {
    if (nSlot == kDamageRankPaletteSlot) {
        return kDamageRankFuncKeyId;
    }
    return CUIKeyConfig__GetIdxFromPaletteSlot(pThis, nSlot);
}

// Required, not cosmetic: DrawFuncKeyMapped and Add/RemoveFromPalette write the in-use flag at
// this + (slot + 0xD3) * 12, and the stock mapping returns slot 55 for type 4 id 55 — past the array.
int __fastcall CUIKeyConfig__GetPaletteSlotFromIdx_hook(void* pThis, void* _EDX, int nType, int nId) {
    if (nType == kDamageRankFuncKeyType && nId == kDamageRankFuncKeyId) {
        return kDamageRankPaletteSlot;
    }
    return CUIKeyConfig__GetPaletteSlotFromIdx(pThis, nType, nId);
}

int __fastcall CWvsContext__ProcessBasicUIKey_hook(void* pThis, void* _EDX, unsigned int nKey, int lParam) {
    const int nResult = CWvsContext__ProcessBasicUIKey(pThis, nKey, lParam);
    if (nResult || (lParam & 0xC0000000) != 0) { // handled, key-up, or auto-repeat
        return nResult;
    }
    // Same early-outs as the original (0x00A07448 / 0x00A0746B).
    auto pUserLocal = *reinterpret_cast<unsigned char**>(0x00BEBF98); // TSingleton<CUserLocal>::ms_pInstance
    if (pUserLocal && *reinterpret_cast<int*>(pUserLocal + 0x3134)) {
        return nResult;
    }
    if (*reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(pThis) + 0x30B0)) {
        return nResult;
    }
    int nScanCode = (lParam >> 16) & 0xFF;
    if (nScanCode == 0x36) {
        nScanCode = 0x2A; // right shift shares the left shift slot, as in the original
    }
    auto pFuncKeyMan = *reinterpret_cast<unsigned char**>(0x00BED5A0); // TSingleton<CFuncKeyMappedMan>::ms_pInstance
    if (!pFuncKeyMan || nScanCode >= kFuncKeyCount) {
        return nResult;
    }
    const unsigned char* pEntry = pFuncKeyMan + 4 + nScanCode * 5; // FUNCKEY_MAPPED { char nType; int nID; }
    int nId;
    memcpy(&nId, pEntry + 1, sizeof(nId));
    if (pEntry[0] != kDamageRankFuncKeyType || nId != kDamageRankFuncKeyId) {
        return nResult;
    }
    ToggleDamageRankFromKey();
    return 1;
}

} // namespace

void AttachDamageRankMod() {
    ATTACH_HOOK(CUIKeyConfig__ResetPaletteItems, CUIKeyConfig__ResetPaletteItems_hook);
    ATTACH_HOOK(CUIKeyConfig__GetIdxFromPaletteSlot, CUIKeyConfig__GetIdxFromPaletteSlot_hook);
    ATTACH_HOOK(CUIKeyConfig__GetPaletteSlotFromIdx, CUIKeyConfig__GetPaletteSlotFromIdx_hook);
    ATTACH_HOOK(CWvsContext__ProcessBasicUIKey, CWvsContext__ProcessBasicUIKey_hook);
    Patch4(0x00833FB0, 0x00BE2C00); // CUIKeyConfig::DrawKeyPalette — loop end cmp [ebp-24h],0BE2BF8h: 40 -> 41 slots (id 55)
    Patch1(0x00BD8D84, 4);          // DefaultFuncKeyMap table 0x00BD8BCC, F12 (scan code 88) nType: Key Config "Default" = DamageRank
    Patch4(0x00BD8D85, 55);         // ... and its nID, matching the server default keymap
    ATTACH_HOOK(CField__OnMouseButton, CField__OnMouseButton_hook);
    ATTACH_HOOK(CField__OnMouseMove, CField__OnMouseMove_hook);
    ATTACH_HOOK(CWnd__OnMouseWheel, CWnd__OnMouseWheel_hook);
}
