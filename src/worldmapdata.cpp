#include "pch.h"
#include "worldmapdata.h"
#include "wvs/util.h"
#include "ztl/ztl.h"
#include <string>
#include <unordered_map>


static std::unordered_map<int, std::string> g_mMapName;
static std::unordered_map<int, std::string> g_mMobName;
static std::unordered_map<int, int> g_mMobLevel;
static std::unordered_map<int, std::string> g_mNpcName;
static std::unordered_map<int, IWzCanvasPtr> g_mMobIcon;
static std::unordered_map<int, IWzCanvasPtr> g_mNpcIcon;
static std::unordered_map<int, IWzCanvasPtr> g_mMapIcon;
static IWzCanvasPtr g_aQuestMarker[4];

static IWzPropertyPtr get_property(const wchar_t* sUOL) {
    try {
        return get_rm()->GetObjectA(sUOL).GetUnknown();
    } catch (...) {
        return nullptr;
    }
}

static IWzPropertyPtr get_child(IWzPropertyPtr pProp, const wchar_t* sName) {
    if (!pProp) {
        return nullptr;
    }
    try {
        return pProp->item[sName].GetUnknown();
    } catch (...) {
        return nullptr;
    }
}

static std::string get_string(IWzPropertyPtr pProp, const wchar_t* sName, const char* sDefault = "Unknown") {
    if (!pProp) {
        return sDefault;
    }
    try {
        Ztl_variant_t v = pProp->item[sName];
        if (v.vt == VT_BSTR) {
            return static_cast<const char*>(_bstr_t(v));
        }
    } catch (...) {
    }
    return sDefault;
}

static IWzCanvasPtr load_canvas(const wchar_t* sUOL) {
    try {
        Ztl_variant_t v = get_rm()->GetObjectA(sUOL);
        IUnknown* pUnknown = v.GetUnknown(false, false);
        if (!pUnknown) {
            return nullptr;
        }
        IWzCanvas* pCanvas = nullptr;
        if (SUCCEEDED(pUnknown->QueryInterface(__uuidof(IWzCanvas), reinterpret_cast<void**>(&pCanvas)))) {
            IWzCanvasPtr pResult = pCanvas;
            pCanvas->Release();
            return pResult;
        }
    } catch (...) {
    }
    return nullptr;
}

std::string GetMapNameById(int nMapID) {
    auto it = g_mMapName.find(nMapID);
    if (it != g_mMapName.end()) {
        return it->second;
    }
    static const wchar_t* asRegion[] = {
        L"maple", L"victoria", L"ossyria", L"elin", L"weddingGL", L"MasteriaGL",
        L"HalloweenGL", L"jp", L"etc", L"singapore", L"event", L"Episode1GL",
    };
    std::string sResult = "Unknown";
    IWzPropertyPtr pRoot = get_property(L"String/Map.img");
    std::wstring sID = std::to_wstring(nMapID);
    for (auto sRegion : asRegion) {
        IWzPropertyPtr pMap = get_child(get_child(pRoot, sRegion), sID.c_str());
        if (pMap) {
            sResult = get_string(pMap, L"streetName") + " - " + get_string(pMap, L"mapName");
            break;
        }
    }
    g_mMapName[nMapID] = sResult;
    return sResult;
}

std::string GetMobNameById(int nMobID) {
    auto it = g_mMobName.find(nMobID);
    if (it != g_mMobName.end()) {
        return it->second;
    }
    std::wstring sID = std::to_wstring(nMobID);
    std::string sName = get_string(get_child(get_property(L"String/Mob.img"), sID.c_str()), L"name");
    g_mMobName[nMobID] = sName;
    return sName;
}

int GetMobLevelById(int nMobID) {
    auto it = g_mMobLevel.find(nMobID);
    if (it != g_mMobLevel.end()) {
        return it->second;
    }
    wchar_t sUOL[64];
    swprintf_s(sUOL, L"Mob/%07d.img", nMobID);
    int nLevel = 0;
    IWzPropertyPtr pInfo = get_child(get_property(sUOL), L"info");
    if (pInfo) {
        try {
            nLevel = get_int32(pInfo->item[L"level"], 0);
        } catch (...) {
        }
    }
    g_mMobLevel[nMobID] = nLevel;
    return nLevel;
}

std::string GetNpcNameById(int nNpcID) {
    auto it = g_mNpcName.find(nNpcID);
    if (it != g_mNpcName.end()) {
        return it->second;
    }
    std::wstring sID = std::to_wstring(nNpcID);
    std::string sName = get_string(get_child(get_property(L"String/Npc.img"), sID.c_str()), L"name");
    g_mNpcName[nNpcID] = sName;
    return sName;
}

IWzCanvasPtr GetMobIcon(int nMobID) {
    auto it = g_mMobIcon.find(nMobID);
    if (it != g_mMobIcon.end()) {
        return it->second;
    }
    wchar_t sUOL[128];
    swprintf_s(sUOL, L"Mob/%07d.img/stand/0", nMobID);
    IWzCanvasPtr pCanvas = load_canvas(sUOL);
    if (!pCanvas) {
        swprintf_s(sUOL, L"Mob/%07d.img/fly/0", nMobID);
        pCanvas = load_canvas(sUOL);
    }
    g_mMobIcon[nMobID] = pCanvas;
    return pCanvas;
}

IWzCanvasPtr GetNpcIcon(int nNpcID) {
    auto it = g_mNpcIcon.find(nNpcID);
    if (it != g_mNpcIcon.end()) {
        return it->second;
    }
    wchar_t sUOL[128];
    swprintf_s(sUOL, L"Npc/%07d.img/stand/0", nNpcID);
    IWzCanvasPtr pCanvas = load_canvas(sUOL);
    g_mNpcIcon[nNpcID] = pCanvas;
    return pCanvas;
}

IWzCanvasPtr GetMapMarkIcon(int nMapID) {
    auto it = g_mMapIcon.find(nMapID);
    if (it != g_mMapIcon.end()) {
        return it->second;
    }
    wchar_t sUOL[128];
    swprintf_s(sUOL, L"Map/Map/Map%d/%09d.img", nMapID / 100000000, nMapID);
    IWzCanvasPtr pCanvas;
    IWzPropertyPtr pInfo = get_child(get_property(sUOL), L"info");
    if (pInfo) {
        try {
            Ztl_variant_t vMark = pInfo->item[L"mapMark"];
            if (vMark.vt == VT_BSTR) {
                std::wstring sMark = L"Map/MapHelper.img/mark/" + std::wstring(V_BSTR(&vMark));
                pCanvas = load_canvas(sMark.c_str());
            }
        } catch (...) {
        }
    }
    g_mMapIcon[nMapID] = pCanvas;
    return pCanvas;
}

IWzCanvasPtr GetQuestMarkerIcon(int nState) {
    if (nState < 1 || nState > 3) {
        return nullptr;
    }
    if (!g_aQuestMarker[nState]) {
        // QuestIcon/0 = bulb (available), /1 = open book (in progress), /2 = brown book (completable)
        const wchar_t* sUOL = nState == 2 ? L"UI/UIWindow.img/QuestIcon/2/0"
                            : nState == 3 ? L"UI/UIWindow.img/QuestIcon/1/0"
                                          : L"UI/UIWindow.img/QuestIcon/0/0";
        g_aQuestMarker[nState] = load_canvas(sUOL);
    }
    return g_aQuestMarker[nState];
}
