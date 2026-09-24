#pragma once
#include "wvs/packet.h"
#include "ztl/ztl.h"

#include <map>
#include <vector>

// Shared state between the in-game renderer / network glue (damageskin.cpp) and the picker window
// (damageskinpicker.cpp). Skins live in Custom.wz at kDamageSkinUOL/<id>.

constexpr const wchar_t* kDamageSkinUOL = L"Custom/BasicEff/BasicEff.img/damageSkin";
constexpr const wchar_t* kDamageSkinUIUOL = L"Custom/UI/UIWindow.img/DamageSkin";

struct DamageSkinProp {
    IWzPropertyPtr pNoRed0;
    IWzPropertyPtr pNoRed1;
    IWzPropertyPtr pNoCri0;
    IWzPropertyPtr pNoCri1;

    // Unit-damage glyphs under NoCustom, only when NoCustom/customType == "glUnit":
    //   NoCustom/NoRed0/0 = "."   /1 = "K"   /2 = "M"   /3 = "B"   (same for NoCri0)
    // bHasCustom is set iff both sheets resolved. The picker's "unit-only" filter and its "1.8K" preview
    // read it.
    IWzPropertyPtr pNoCustomRed0;
    IWzPropertyPtr pNoCustomCri0;
    bool bHasCustom = false;

    // CUnitPropertyWrapper instances (damageskin.cpp), built once when bHasCustom. Never freed; they
    // live for the DLL's lifetime.
    void* pWrapNormal = nullptr;
    void* pWrapCrit = nullptr;
};

extern std::map<int, DamageSkinProp> g_mDamageSkinProp;
extern std::vector<int> g_vSkinIds;

// The local character's applied skin (0 = stock digits), as confirmed by the server.
extern int g_nActiveSkin;

// Idempotent. Loads the skin tree from Custom.wz on the first call.
void LoadDamageSkin();

// Shop catalog and owned skins, pushed by the server at login and kept fresh by later packets.
struct DamageSkinCatalogEntry {
    int nID;
    long long llPrice;
};
extern std::vector<DamageSkinCatalogEntry> g_vShopCatalog;
extern std::vector<int> g_vOwnedSkins;

// Requests. Fire-and-forget; the server answers with DAMAGE_SKIN_RESULT.
void Send_DamageSkinApply(int nSkinId);
void Send_DamageSkinPurchase(int nSkinId);

// Rebuilds the picker's lists if it is open (damageskinpicker.cpp).
void RefreshDamageSkinPicker();

namespace damageskin {
// Server -> client opcodes, dispatched from clientsocket.cpp. Must match Server SendOpcode.
constexpr unsigned short LP_CATALOG = 0x170;
constexpr unsigned short LP_INVENTORY = 0x171;
constexpr unsigned short LP_RESULT = 0x172;
constexpr unsigned short LP_BROADCAST = 0x173;

void HandlePacket(CInPacket* pPacket);
}
