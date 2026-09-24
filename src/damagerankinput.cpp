#include "pch.h"
#include "hook.h"
#include "uiDamageRank.h"
#include "wvs/field.h"

// DamageRank input: F12 toggle plus field mouse forwarding. The window is a bare Gr2D layer, not a
// CWnd, so it only sees input the field would otherwise get; drag/wheel fallbacks live in bypass.cpp.

namespace {

auto CField__OnKey = CField::OnKey;

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

void __fastcall CField__OnKey_hook(CField* pThis, void* _EDX, unsigned int wParam, int lParam) {
    const bool bKeyUp = (lParam & 0x80000000) != 0;
    const bool bRepeat = (lParam & 0x40000000) != 0;
    if (wParam == VK_F12 && !bKeyUp && !bRepeat) {
        auto& ui = CUIDamageRank::GetInstance();
        const bool bWasVisible = ui.IsVisible();
        CUIDamageRank::ToggleByHotkey();
        if (!bWasVisible && ui.IsVisible()) {
            CUIDamageRank::PlayUISound(L"MenuUp");
        }
        return;
    }
    CField__OnKey(pThis, wParam, lParam);
}

} // namespace

void AttachDamageRankMod() {
    ATTACH_HOOK(CField__OnKey, CField__OnKey_hook);
    ATTACH_HOOK(CField__OnMouseButton, CField__OnMouseButton_hook);
    ATTACH_HOOK(CField__OnMouseMove, CField__OnMouseMove_hook);
    ATTACH_HOOK(CWnd__OnMouseWheel, CWnd__OnMouseWheel_hook);
}
