#include "pch.h"
#include "hook.h"
#include "clientsocket.h"
#include "worldmapdata.h"
#include "wvs/field.h"
#include "wvs/packet.h"
#include "ztl/ztl.h"
#include <string>

// Hyper Teleport Rock. Left-clicking a world map spot while owning item 5590001 asks for confirmation
// and sends CP 0x105 [int targetMapId]; the server (HyperTeleportRockHandler) validates ownership,
// expiry and map rules, then warps. The rock is not consumed. The client check here is UX only.
// Ported from the kaentake-main "hyper-teleport-rock" mod; every address re-verified against this exe.

constexpr unsigned short CP_HYPER_TELEPORT_ROCK = 0x105; // Server RecvOpcode.HYPER_TELEPORT_ROCK
constexpr int HYPER_TELEPORT_ROCK_ITEM_ID = 5590001;
constexpr int YESNO_YES = 6;       // CUtilDlg::YesNo result, every stock caller tests == 6
constexpr int DIALOG_RET_CLOSE = 2; // CDialog::OnKey sends SetRet(2) on ESC

// CWorldMapDlg::OnMouseButton — v95 sym, v83 VA 0x009EE6E5 (IUIMsgHandler, this = dlg + 4, ret 0x10).
// WM_LBUTTONUP runs SetWorldMapDeeper, WM_RBUTTONUP runs SetWorldMapShallower.
static auto CWorldMapDlg__OnMouseButton = reinterpret_cast<void(__thiscall*)(void*, unsigned int, unsigned int, int, int)>(0x009EE6E5);
// CDialog::SetRet — v95 sym, v83 VA 0x004B5F05 (unnamed in the IDB; CWorldMapDlg vtable 0xB3F1E8 slot +0x34, ret 4)
static auto CDialog__SetRet = reinterpret_cast<void(__thiscall*)(void*, int)>(0x004B5F05);
// CWvsContext::GetItemCount — v95 sym, v83 VA 0x00A2858C (ret 4; reads TSingleton<CWvsContext> 0x00BE7918 itself)
static auto CWvsContext__GetItemCount = reinterpret_cast<int(__thiscall*)(void*, int)>(0x00A2858C);
constexpr uintptr_t CWvsContext_Inst = 0x00BE7918;
// CUtilDlg::YesNo / CUtilDlg::Notice — v95 sym, v83 VA 0x00992BFD / 0x009929DD
// (cdecl: msg by value, sound, ZRef<CDialog>*, autoSeparated, int; the callee releases the ZXString)
static auto CUtilDlg__YesNo = reinterpret_cast<int(__cdecl*)(ZXString<char>, const wchar_t*, void*, int, int)>(0x00992BFD);
static auto CUtilDlg__Notice = reinterpret_cast<int(__cdecl*)(ZXString<char>, const wchar_t*, void*, int, int)>(0x009929DD);

static bool HasHyperTeleportRock() {
    void* pContext = *reinterpret_cast<void**>(CWvsContext_Inst);
    return pContext && CWvsContext__GetItemCount(pContext, HYPER_TELEPORT_ROCK_ITEM_ID) > 0;
}

static bool ConfirmMove(int nMapID) {
    std::string sName = GetMapNameById(nMapID);
    if (sName.empty() || sName == "Unknown") {
        sName = std::to_string(nMapID);
    }
    ZXString<char> sText;
    sText.Format("Are you sure you want to move to\r\n%s?", sName.c_str());
    return CUtilDlg__YesNo(sText, nullptr, nullptr, 1, 0) == YESNO_YES;
}

// Returns true when the click landed on a spot and was consumed.
static bool TryHyperTeleport(char* pDlg, int rx, int ry) {
    int nMapID = 0;
    try {
        nMapID = FindWorldMapSpotMapID(pDlg, rx, ry);
    } catch (...) {
    }
    if (nMapID <= 0) {
        return false;
    }
    if (!HasHyperTeleportRock()) {
        CUtilDlg__Notice(ZXString<char>("You need a Hyper Teleport Rock to move there."), nullptr, nullptr, 0, 0);
        return true;
    }
    if (!ConfirmMove(nMapID)) {
        return true;
    }
    COutPacket oPacket(CP_HYPER_TELEPORT_ROCK);
    oPacket.Encode4(static_cast<unsigned int>(nMapID));
    SendClientPacket(oPacket);
    CDialog__SetRet(pDlg, DIALOG_RET_CLOSE); // close the world map, same as ESC
    return true;
}

static void __fastcall CWorldMapDlg__OnMouseButton_hook(void* pThis, void* _EDX, unsigned int uMsg, unsigned int wParam, int rx, int ry) {
    if (uMsg == WM_LBUTTONUP && get_field() && TryHyperTeleport(reinterpret_cast<char*>(pThis) - 4, rx, ry)) {
        return;
    }
    CWorldMapDlg__OnMouseButton(pThis, uMsg, wParam, rx, ry);
}

void AttachHyperTeleportRockMod() {
    ATTACH_HOOK(CWorldMapDlg__OnMouseButton, CWorldMapDlg__OnMouseButton_hook);
}
