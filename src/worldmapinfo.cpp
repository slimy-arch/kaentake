#include "pch.h"
#include "hook.h"
#include "worldmapdata.h"
#include "wvs/field.h"
#include "wvs/msghandler.h"
#include "wvs/packet.h"
#include "wvs/tooltip.h"
#include "wvs/util.h"
#include "ztl/ztl.h"
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#undef min
#undef max

// World map "Map Info" tooltip. Hovering a map spot replaces the stock tooltip with a panel listing
// the map's monsters and NPCs (from WZ), quest markers for those NPCs, and the players currently in
// that map on any channel (server-fed, CP 0x115 -> LP 0x178, see Server WorldMapPlayersHandler).
// Ported from the kaentake-main "worldmap-tooltip" mod; every address re-verified against this exe.


// =====================================================================================================
// Client addresses
// =====================================================================================================

// CWorldMapDlg::OnMouseMove — v95 sym, v83 VA 0x009EE2B3 (IUIMsgHandler vtbl slot 3, this = dlg + 4, ret 8)
static auto CWorldMapDlg__OnMouseMove = reinterpret_cast<int(__thiscall*)(void*, int, int)>(0x009EE2B3);
// CWorldMapDlg::OnDestroy — v95 sym, v83 VA 0x009EB94A
static auto CWorldMapDlg__OnDestroy = reinterpret_cast<void(__thiscall*)(void*)>(0x009EB94A);

// CWorldMapDlg layout, from CWorldMapDlg::CheckSpotInfo 0x009EDD67 and LoadMapList 0x009ED38B
namespace wm {
constexpr size_t OFF_MARK_CANVAS = 0x5B4; // IWzCanvas*[] — spot mark canvases, indexed by spot+0x08
constexpr size_t OFF_SPOTS = 0x5B8;       // spot array (count at [-4]), stride 0x44
constexpr size_t SPOT_STRIDE = 0x44;
constexpr size_t SPOT_X = 0x00;
constexpr size_t SPOT_Y = 0x04;
constexpr size_t SPOT_MARK = 0x08;
constexpr size_t SPOT_MAPNO = 0x2C;       // ZArray<long> of "mapNo" entries
constexpr int ORIGIN_X = 13;              // CheckSpotInfo: rx - 0xD
constexpr int ORIGIN_Y = 35;              // CheckSpotInfo: ry - 0x23
constexpr int FALLBACK_HIT = 8;
constexpr int TOOLTIP_OFFSET = 0x14;      // CheckSpotInfo: GetAbsLeft() + rx + 0x14
}

// CNpc::SetQuestList 0x006D1779 is the reference for the quest marker classification below
namespace quest {
constexpr uintptr_t CWvsContext_Inst = 0x00BE7918;   // TSingleton<CWvsContext>::ms_pInstance
constexpr uintptr_t CQuestMan_Inst = 0x00BED614;     // TSingleton<CQuestMan>::ms_pInstance
constexpr size_t OFF_CHARACTERDATA = 0x20B8;         // CWvsContext -> CharacterData* (see 0x00425D0B)
constexpr size_t OFF_SECONDARYSTAT = 0x2134;         // &CWvsContext::m_secondaryStat
constexpr size_t OFF_TAMINGMOBLEVEL = 0x37C0;        // CWvsContext int
constexpr size_t OFF_QUEST_INPROGRESS = 0x5FF;       // CharacterData ZMap<u16, ZXString<char>> (unaligned, as the client uses it)
constexpr unsigned short QUEST_BAND_LO = 0x4B0;      // SetQuestList skips [0x4B0, 0x578)
constexpr unsigned short QUEST_BAND_HI = 0x578;

// CWvsContext::GetCurFieldID — v95 sym, v83 VA 0x00A1238B
static auto GetCurFieldID = reinterpret_cast<long(__thiscall*)(void*)>(0x00A1238B);
// CQuestMan::GetQuestByNpc — v95 sym, v83 VA 0x0071DDEC (ret 8)
static auto GetQuestByNpc = reinterpret_cast<int(__thiscall*)(void*, unsigned int, ZArray<unsigned short>*)>(0x0071DDEC);
// ZMap<unsigned short, ...>::GetPos — v83 VA 0x00500EA5 (ret 4), unnamed in the IDB
static auto ZMap_GetPos = reinterpret_cast<void*(__thiscall*)(void*, unsigned short*)>(0x00500EA5);
// CQuestMan::CheckStartDemand — v95 sym, v83 VA 0x00721163 (ret 0x18: 6 args in v83, the IDB name is v95's 8)
static auto CheckStartDemand = reinterpret_cast<int(__thiscall*)(void*, unsigned short, unsigned int, void*, void*, int, int)>(0x00721163);
// CQuestMan::CheckCompleteDemand — v95 sym, v83 VA 0x00721D2C (ret 0x10, returns 0 when completable)
static auto CheckCompleteDemand = reinterpret_cast<long(__thiscall*)(void*, unsigned short, unsigned int, void*, void*)>(0x00721D2C);
}

namespace players {
constexpr unsigned short CP_WORLD_MAP_PLAYERS = 0x115; // Server RecvOpcode.WORLD_MAP_PLAYERS
constexpr uintptr_t CClientSocket_Inst = 0x00BE7914;   // TSingleton<CClientSocket>::ms_pInstance
// CClientSocket::SendPacket — v95 sym, v83 VA 0x0049637B (ret 4)
static auto CClientSocket__SendPacket = reinterpret_cast<void(__thiscall*)(void*, const COutPacket*)>(0x0049637B);
}


// =====================================================================================================
// Layout
// =====================================================================================================

namespace ui {
constexpr int MIN_WIDTH = 200;
constexpr int MAX_WIDTH = 460;
constexpr int MAX_HEIGHT = 600;

constexpr int LEFT_PAD = 14;
constexpr int RIGHT_PAD = 14;
constexpr int TOP_PAD = 8;
constexpr int BOTTOM_PAD = 8;

constexpr int HEADER_HEIGHT = 56;   // map mark + street/name block
constexpr int SECTION_LABEL_H = 18; // "Monsters" / "NPCs" / "Players" label row
constexpr int LINE_HEIGHT = 46;     // mob/NPC row
constexpr int PLAYER_LINE_H = 18;   // text-only player row
constexpr int CATEGORY_GAP = 6;

constexpr int ICON_SIZE = 42;
constexpr int ICON_COL_W = 52;
constexpr int MARKER_SIZE = 22;
constexpr int CHAR_W = 7;           // approximate glyph width for 12pt Dotum

constexpr int MAX_ENTRIES = 10;

constexpr int CHROME_H = 28;
constexpr int TITLE_BAR_H = 20;
constexpr int TITLE_TEXT_Y = 7;

constexpr unsigned int COL_BG = 0x99141A24;
constexpr unsigned int COL_TITLEBAR = 0x88423010;
constexpr unsigned int COL_SEPARATOR = 0xCCE0C070;
}


// =====================================================================================================
// Map data
// =====================================================================================================

struct MobGroup {
    int nID;
    int nLevel;
    int nCount;
    std::string sName;
};

struct NpcEntry {
    int nID;
    std::string sName;
    int nQuestState; // 0 none, 1 available, 2 completable, 3 in progress
};

struct PlayerEntry {
    std::string sName;
    int nLevel;
    int nChannel;
};

struct MapLife {
    std::vector<MobGroup> aMob;
    std::vector<NpcEntry> aNpc;
    std::vector<PlayerEntry> aPlayer;
    int nPlayerState = 0; // 0 not requested, 1 loading, 2 ready, 3 no reply from the server
};

namespace quest {

static int GetNpcQuestStateInner(int nNpcID) {
    void* pQuestMan = *reinterpret_cast<void**>(CQuestMan_Inst);
    char* pContext = *reinterpret_cast<char**>(CWvsContext_Inst);
    if (!pQuestMan || !pContext) {
        return 0;
    }
    char* pCharacterData = *reinterpret_cast<char**>(pContext + OFF_CHARACTERDATA);
    if (!pCharacterData) {
        return 0;
    }
    void* pSecondaryStat = pContext + OFF_SECONDARYSTAT;
    int nTamingMobLevel = *reinterpret_cast<int*>(pContext + OFF_TAMINGMOBLEVEL);
    int nFieldID = static_cast<int>(GetCurFieldID(pContext));

    ZArray<unsigned short> aQuest;
    GetQuestByNpc(pQuestMan, static_cast<unsigned int>(nNpcID), &aQuest);
    size_t uCount = aQuest.GetCount();
    if (uCount == 0 || uCount > 4096) {
        return 0;
    }

    void* pInProgress = pCharacterData + OFF_QUEST_INPROGRESS;
    bool bAvailable = false;
    bool bInProgress = false;
    for (size_t i = 0; i < uCount; ++i) {
        unsigned short usQuestID = aQuest[i];
        if (usQuestID >= QUEST_BAND_LO && usQuestID < QUEST_BAND_HI) {
            continue;
        }
        if (ZMap_GetPos(pInProgress, &usQuestID)) {
            if (CheckCompleteDemand(pQuestMan, usQuestID, nNpcID, pCharacterData, pSecondaryStat) == 0) {
                return 2; // completable takes priority, as in SetQuestList
            }
            bInProgress = true;
        } else if (CheckStartDemand(pQuestMan, usQuestID, nNpcID, pCharacterData, pSecondaryStat, nTamingMobLevel, nFieldID)) {
            bAvailable = true;
        }
    }
    return bAvailable ? 1 : (bInProgress ? 3 : 0);
}

static int GetNpcQuestState(int nNpcID) {
    __try {
        return GetNpcQuestStateInner(nNpcID);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

}

static void RedrawIfShown(int nMapID);

namespace players {

constexpr DWORD REPLY_TIMEOUT_MS = 3000; // unanswered for this long -> show "no response"
constexpr DWORD TTL_MS = 6000;     // a reply is fresh for 6s
constexpr DWORD MIN_REQ_MS = 2500; // never re-request the same map faster than this
constexpr int MAX_PLAYERS = 40;    // must match WorldMapPlayersHandler.MAX_PLAYERS
constexpr size_t MAX_CACHE = 64;

struct Entry {
    std::vector<PlayerEntry> aPlayer;
    DWORD tStamp = 0;
    DWORD tRequest = 0;
    DWORD tPending = 0; // first request since the last reply, 0 when none is outstanding
    bool bReady = false;
};

// OnMouseMove and ProcessPacket both run on the client's main thread, so no locking is needed.
static std::unordered_map<int, Entry> g_mCache;

static void EvictStale(DWORD tNow) {
    if (g_mCache.size() <= MAX_CACHE) {
        return;
    }
    for (auto it = g_mCache.begin(); it != g_mCache.end();) {
        if (!it->second.bReady || tNow - it->second.tStamp > TTL_MS) {
            it = g_mCache.erase(it);
        } else {
            ++it;
        }
    }
}

static void MaybeRequest(int nMapID) {
    void* pSocket = *reinterpret_cast<void**>(CClientSocket_Inst);
    if (nMapID <= 0 || !pSocket) {
        return;
    }
    DWORD tNow = GetTickCount();
    EvictStale(tNow);
    Entry& e = g_mCache[nMapID];
    bool bStale = !e.bReady || tNow - e.tStamp > TTL_MS;
    bool bCooled = e.tRequest == 0 || tNow - e.tRequest >= MIN_REQ_MS;
    if (!bStale || !bCooled) {
        return;
    }
    e.tRequest = tNow;
    if (e.tPending == 0) {
        e.tPending = tNow;
    }
    COutPacket oPacket(CP_WORLD_MAP_PLAYERS);
    oPacket.Encode4(static_cast<unsigned int>(nMapID));
    CClientSocket__SendPacket(pSocket, &oPacket);
}

static void GetCached(int nMapID, std::vector<PlayerEntry>& aOut, int& nState) {
    auto it = g_mCache.find(nMapID);
    if (it == g_mCache.end()) {
        nState = 0;
        return;
    }
    aOut = it->second.aPlayer;
    const Entry& e = it->second;
    if (e.bReady) {
        nState = 2;
    } else {
        nState = e.tPending && GetTickCount() - e.tPending > REPLY_TIMEOUT_MS ? 3 : 1;
    }
}

// LP 0x178, routed here by clientsocket.cpp with the offset still on the opcode.
// int mapId, short count, { string name, short level, byte channel } x count
void HandleResponse(CInPacket* pPacket) {
    pPacket->SetOffset(pPacket->GetOffset() + 2);
    int nMapID = pPacket->Decode<int>();
    if (nMapID <= 0) {
        return;
    }
    int nCount = std::min<int>(pPacket->Decode<unsigned short>(), MAX_PLAYERS);
    std::vector<PlayerEntry> aPlayer;
    for (int i = 0; i < nCount; ++i) {
        if (!pPacket->CanRead(2)) {
            break;
        }
        unsigned short usLen = pPacket->Decode<unsigned short>();
        if (usLen == 0 || usLen > 64 || !pPacket->CanRead(usLen + 3)) {
            break;
        }
        std::string sName(reinterpret_cast<const char*>(pPacket->Current()), usLen);
        pPacket->SetOffset(pPacket->GetOffset() + usLen);
        int nLevel = pPacket->Decode<unsigned short>();
        int nChannel = pPacket->Decode<unsigned char>();
        aPlayer.push_back({ std::move(sName), nLevel, nChannel });
    }
    Entry& e = g_mCache[nMapID];
    e.aPlayer = std::move(aPlayer);
    e.tStamp = GetTickCount();
    e.tPending = 0;
    e.bReady = true;
    RedrawIfShown(nMapID); // show the reply now instead of waiting for the next mouse move
}

}

static void ComputeMapWzLife(int nMapID, MapLife& out) {
    wchar_t sUOL[128];
    swprintf_s(sUOL, L"Map/Map/Map%d/%09d.img/life", nMapID / 100000000, nMapID);
    IWzPropertyPtr pLife;
    try {
        pLife = get_rm()->GetObjectA(sUOL).GetUnknown();
    } catch (...) {
    }
    if (!pLife) {
        return;
    }

    std::unordered_map<int, MobGroup> mMob;
    std::unordered_set<int> setNpc;
    int nLifeCount = 0;
    try {
        nLifeCount = static_cast<int>(pLife->Getcount());
    } catch (...) {
    }
    for (int i = 0; i < nLifeCount; ++i) {
        try {
            wchar_t sIndex[16];
            _itow_s(i, sIndex, 10);
            IWzPropertyPtr pEntry = get_unknown(pLife->item[sIndex]);
            if (!pEntry) {
                continue;
            }
            Ztl_variant_t vType = pEntry->item[L"type"];
            if (vType.vt != VT_BSTR) {
                continue;
            }
            int nID = get_int32(pEntry->item[L"id"], 0);
            if (wcscmp(V_BSTR(&vType), L"m") == 0) {
                MobGroup& m = mMob[nID];
                if (m.nCount == 0) {
                    m = { nID, GetMobLevelById(nID), 0, GetMobNameById(nID) };
                }
                m.nCount++;
            } else if (wcscmp(V_BSTR(&vType), L"n") == 0 && setNpc.insert(nID).second) {
                out.aNpc.push_back({ nID, GetNpcNameById(nID), quest::GetNpcQuestState(nID) });
            }
        } catch (...) {
            // skip a malformed life entry, keep the rest
        }
    }

    for (auto& kv : mMob) {
        out.aMob.push_back(kv.second);
    }
    std::sort(out.aMob.begin(), out.aMob.end(), [](const MobGroup& a, const MobGroup& b) {
        return a.nLevel < b.nLevel;
    });
    if (out.aMob.size() > ui::MAX_ENTRIES) {
        out.aMob.resize(ui::MAX_ENTRIES);
    }
    if (out.aNpc.size() > ui::MAX_ENTRIES) {
        out.aNpc.resize(ui::MAX_ENTRIES);
    }
}

static MapLife GetMapLife(int nMapID) {
    // OnMouseMove fires per pixel; only re-walk WZ and re-run the quest checks when the map changes.
    static int s_nLastMapID = -1;
    static MapLife s_lastLife;
    if (nMapID != s_nLastMapID) {
        MapLife fresh;
        ComputeMapWzLife(nMapID, fresh);
        s_lastLife = std::move(fresh);
        s_nLastMapID = nMapID;
    }
    MapLife out;
    out.aMob = s_lastLife.aMob;
    out.aNpc = s_lastLife.aNpc;
    players::GetCached(nMapID, out.aPlayer, out.nPlayerState);
    return out;
}


// =====================================================================================================
// Rendering
// =====================================================================================================

alignas(8) static unsigned char g_abToolTip[0x600]; // CUIToolTip is at most 0x514 bytes (CWorldMapDlg+0x88..+0x59C)
static bool g_bToolTipInit = false;

static CUIToolTip* GetToolTip() {
    if (!g_bToolTipInit) {
        reinterpret_cast<void(__thiscall*)(void*)>(0x008E49B5)(g_abToolTip); // CUIToolTip::CUIToolTip
        g_bToolTipInit = true;
    }
    return reinterpret_cast<CUIToolTip*>(g_abToolTip);
}

static int g_nShownMapID = 0; // map whose tooltip is on screen, with the cursor it was placed at
static int g_nShownX = 0;
static int g_nShownY = 0;

static void ClearMapInfoToolTip() {
    g_nShownMapID = 0;
    if (g_bToolTipInit) {
        reinterpret_cast<CUIToolTip*>(g_abToolTip)->ClearToolTip();
    }
}

static IWzFontPtr g_pFontTitle;
static IWzFontPtr g_pFontMapName;
static IWzFontPtr g_pFontMonsters;
static IWzFontPtr g_pFontNpcs;
static IWzFontPtr g_pFontPlayers;
static IWzFontPtr g_pFontBody;
static IWzFontPtr g_pFontShadow;

static void CreateFont(IWzFontPtr& pFont, unsigned int uColor, unsigned int uHeight) {
    if (pFont) {
        return;
    }
    try {
        PcCreateObject<IWzFontPtr>(L"Canvas#Font", pFont, nullptr);
        if (pFont) {
            pFont->Create(L"Dotum", uHeight, uColor, Ztl_variant_t(L"B"));
        }
    } catch (...) {
        pFont = nullptr;
    }
}

static void EnsureFonts() {
    CreateFont(g_pFontTitle, 0xFFFFD45A, 12);
    CreateFont(g_pFontMapName, 0xFF6EDB7E, 13);
    CreateFont(g_pFontMonsters, 0xFFFF6A6A, 12);
    CreateFont(g_pFontNpcs, 0xFF6FAFFF, 12);
    CreateFont(g_pFontPlayers, 0xFFC9A0FF, 12);
    CreateFont(g_pFontBody, 0xFFFFFFFF, 12);
    CreateFont(g_pFontShadow, 0xFF000000, 12);
}

static std::string FormatMob(const MobGroup& m) {
    return "[Lv. " + std::to_string(m.nLevel) + "] " + m.sName + " (x" + std::to_string(m.nCount) + ")";
}

static std::string FormatPlayer(const PlayerEntry& p) {
    return "[Lv." + std::to_string(p.nLevel) + "] " + p.sName + "  (Ch." + std::to_string(p.nChannel) + ")";
}

static int ComputeWidth(const MapLife& data, const std::string& sStreet, const std::string& sMap) {
    int nWidth = ui::LEFT_PAD + ui::ICON_SIZE + 8 + static_cast<int>(std::max(sStreet.size(), sMap.size())) * ui::CHAR_W + ui::RIGHT_PAD;
    for (auto& m : data.aMob) {
        nWidth = std::max(nWidth, ui::LEFT_PAD + ui::ICON_COL_W + static_cast<int>(FormatMob(m).size()) * ui::CHAR_W + ui::RIGHT_PAD);
    }
    for (auto& n : data.aNpc) {
        nWidth = std::max(nWidth, ui::LEFT_PAD + ui::ICON_COL_W + static_cast<int>(n.sName.size()) * ui::CHAR_W + ui::RIGHT_PAD);
    }
    for (auto& p : data.aPlayer) {
        nWidth = std::max(nWidth, ui::LEFT_PAD + static_cast<int>(FormatPlayer(p).size()) * ui::CHAR_W + ui::RIGHT_PAD);
    }
    if (data.nPlayerState == 3) {
        nWidth = std::max(nWidth, ui::LEFT_PAD + static_cast<int>(strlen("Players (no response from server)")) * ui::CHAR_W + ui::RIGHT_PAD);
    }
    return std::max(ui::MIN_WIDTH, std::min(nWidth, ui::MAX_WIDTH));
}

static int ComputeHeight(const MapLife& data) {
    bool bMob = !data.aMob.empty();
    bool bNpc = !data.aNpc.empty();
    int nHeight = ui::CHROME_H + ui::TOP_PAD + ui::HEADER_HEIGHT;
    if (bMob) {
        nHeight += ui::SECTION_LABEL_H + static_cast<int>(data.aMob.size()) * ui::LINE_HEIGHT;
    }
    if (bMob && bNpc) {
        nHeight += ui::CATEGORY_GAP;
    }
    if (bNpc) {
        nHeight += ui::SECTION_LABEL_H + static_cast<int>(data.aNpc.size()) * ui::LINE_HEIGHT;
    }
    if (data.nPlayerState != 0) {
        if (bMob || bNpc) {
            nHeight += ui::CATEGORY_GAP;
        }
        int nRows = data.nPlayerState == 2 ? std::max(1, static_cast<int>(data.aPlayer.size())) : 0; // empty -> one "No players" row
        nHeight += ui::SECTION_LABEL_H + nRows * ui::PLAYER_LINE_H;
    }
    nHeight += ui::BOTTOM_PAD;
    return std::min(nHeight, ui::MAX_HEIGHT);
}

static void DrawScaled(IWzCanvasPtr pCanvas, IWzCanvasPtr pIcon, int x, int y, int nMaxSize) {
    if (!pIcon) {
        return;
    }
    int w = static_cast<int>(pIcon->width);
    int h = static_cast<int>(pIcon->height);
    if (w <= 0 || h <= 0) {
        return;
    }
    int nw = w >= h ? nMaxSize : w * nMaxSize / h;
    int nh = h >= w ? nMaxSize : h * nMaxSize / w;
    pCanvas->CopyEx(x, y, pIcon, CA_OVERWRITE, nw, nh, 0, 0, w, h);
}

static void DrawShadowText(IWzCanvasPtr pCanvas, int x, int y, const std::string& s, IWzFontPtr pFont) {
    if (!pFont || !g_pFontShadow) {
        return;
    }
    pCanvas->DrawTextA(x + 1, y + 1, s.c_str(), g_pFontShadow);
    pCanvas->DrawTextA(x, y, s.c_str(), pFont);
}

static int CenterX(const std::string& s, int nWidth) {
    return (nWidth - static_cast<int>(s.size()) * ui::CHAR_W) / 2;
}

static void ShowMapInfoToolTip(int nMapID, int nCursorX, int nCursorY) {
    std::string sFull = GetMapNameById(nMapID);
    size_t uSplit = sFull.find(" - ");
    std::string sStreet = sFull.substr(0, uSplit);
    std::string sMap = uSplit != std::string::npos ? sFull.substr(uSplit + 3) : sFull;

    EnsureFonts();
    CUIToolTip* pToolTip = GetToolTip();
    MapLife data = GetMapLife(nMapID);
    int nWidth = ComputeWidth(data, sStreet, sMap);
    int nHeight = ComputeHeight(data);

    // Same anchor as CWorldMapDlg::CheckSpotInfo, flipped to stay on screen
    int x = nCursorX + wm::TOOLTIP_OFFSET;
    int y = nCursorY + wm::TOOLTIP_OFFSET;
    if (x + nWidth > get_screen_width()) {
        x = nCursorX - nWidth - 4;
    }
    if (y + nHeight > get_screen_height()) {
        y = nCursorY - nHeight - 4;
    }
    x = std::max(x, 0);
    y = std::max(y, 0);

    // Size the native box by blank description lines: two probes give the per-line height, then
    // pick the smallest line count that reaches nHeight.
    ZXString<char> sTitle("");
    auto setLines = [&](int nLines) -> int {
        std::string sDesc(std::max(0, nLines), '\n');
        pToolTip->ClearToolTip();
        pToolTip->SetToolTip_String2(x, y, sTitle, ZXString<char>(sDesc.c_str()), 0, 0, 0, nWidth, 1, 0);
        return pToolTip->m_nHeight;
    };
    int h2 = setLines(2);
    int h12 = setLines(12);
    int nPerLine = h12 > h2 ? std::max(1, (h12 - h2) / 10) : 14;
    int nBase = h2 - 2 * nPerLine;
    int nLines = std::max(1, (nHeight - nBase + nPerLine - 1) / nPerLine);
    setLines(nLines);
    for (int i = 0; i < 4 && pToolTip->m_nHeight < nHeight; ++i) {
        setLines(++nLines);
    }

    if (!pToolTip->m_pLayer) {
        return;
    }
    try {
        IWzCanvasPtr pCanvas = pToolTip->m_pLayer->canvas[0];
        if (!pCanvas) {
            return;
        }
        int nBoxW = pToolTip->m_nWidth;
        int nBoxH = pToolTip->m_nHeight;

        pCanvas->DrawRectangle(2, 2, nBoxW - 4, nBoxH - 4, ui::COL_BG);
        pCanvas->DrawRectangle(4, 4, nBoxW - 8, ui::TITLE_BAR_H, ui::COL_TITLEBAR);
        pCanvas->DrawRectangle(8, 4 + ui::TITLE_BAR_H + 2, nBoxW - 16, 1, ui::COL_SEPARATOR);
        DrawShadowText(pCanvas, CenterX("Map Info", nBoxW), ui::TITLE_TEXT_Y, "Map Info", g_pFontTitle);

        int nHeaderX = ui::LEFT_PAD;
        int nHeaderY = ui::CHROME_H + ui::TOP_PAD;
        IWzCanvasPtr pMark = GetMapMarkIcon(nMapID);
        if (pMark) {
            DrawScaled(pCanvas, pMark, nHeaderX, nHeaderY, ui::ICON_SIZE);
            nHeaderX += ui::ICON_SIZE + 8;
        }
        DrawShadowText(pCanvas, nHeaderX, nHeaderY + 4, sStreet, g_pFontMapName);
        DrawShadowText(pCanvas, nHeaderX, nHeaderY + 20, sMap, g_pFontMapName);

        int nY = ui::CHROME_H + ui::TOP_PAD + ui::HEADER_HEIGHT;
        auto drawLabel = [&](const std::string& sLabel, IWzFontPtr pFont) {
            DrawShadowText(pCanvas, CenterX(sLabel, nBoxW), nY, sLabel, pFont);
            nY += ui::SECTION_LABEL_H;
        };

        if (!data.aMob.empty()) {
            drawLabel("Monsters (" + std::to_string(data.aMob.size()) + "):", g_pFontMonsters);
            for (auto& m : data.aMob) {
                DrawScaled(pCanvas, GetMobIcon(m.nID), ui::LEFT_PAD, nY + 2, ui::ICON_SIZE);
                DrawShadowText(pCanvas, ui::LEFT_PAD + ui::ICON_COL_W, nY + 16, FormatMob(m), g_pFontBody);
                nY += ui::LINE_HEIGHT;
            }
        }
        if (!data.aMob.empty() && !data.aNpc.empty()) {
            nY += ui::CATEGORY_GAP;
        }
        if (!data.aNpc.empty()) {
            drawLabel("NPCs (" + std::to_string(data.aNpc.size()) + "):", g_pFontNpcs);
            for (auto& n : data.aNpc) {
                DrawScaled(pCanvas, GetNpcIcon(n.nID), ui::LEFT_PAD, nY + 2, ui::ICON_SIZE);
                if (n.nQuestState) {
                    int mx = ui::LEFT_PAD + ui::ICON_SIZE - ui::MARKER_SIZE + 2;
                    IWzCanvasPtr pMarker = GetQuestMarkerIcon(n.nQuestState);
                    if (pMarker) {
                        DrawScaled(pCanvas, pMarker, mx, nY, ui::MARKER_SIZE);
                    } else {
                        unsigned int uColor = n.nQuestState == 2 ? 0xFF8B5A2B : n.nQuestState == 3 ? 0xFFE0A050 : 0xFFF0F0F0;
                        pCanvas->DrawRectangle(mx, nY, 10, 10, uColor);
                    }
                }
                DrawShadowText(pCanvas, ui::LEFT_PAD + ui::ICON_COL_W, nY + 16, n.sName, g_pFontBody);
                nY += ui::LINE_HEIGHT;
            }
        }
        if (data.nPlayerState != 0) {
            if (!data.aMob.empty() || !data.aNpc.empty()) {
                nY += ui::CATEGORY_GAP;
            }
            std::string sLabel = data.nPlayerState == 2 ? "Players (" + std::to_string(data.aPlayer.size()) + "):"
                               : data.nPlayerState == 3 ? "Players (no response from server)"
                                                        : "Players (loading...)";
            drawLabel(sLabel, g_pFontPlayers);
            if (data.nPlayerState == 2 && data.aPlayer.empty()) {
                DrawShadowText(pCanvas, CenterX("No players in this map", nBoxW), nY, "No players in this map", g_pFontBody);
                nY += ui::PLAYER_LINE_H;
            }
            for (auto& p : data.aPlayer) {
                DrawShadowText(pCanvas, ui::LEFT_PAD, nY, FormatPlayer(p), g_pFontBody);
                nY += ui::PLAYER_LINE_H;
            }
        }
    } catch (...) {
        // a failed COM draw call must never take the client down
    }
}


static void RedrawIfShown(int nMapID) {
    if (nMapID == g_nShownMapID) {
        ShowMapInfoToolTip(g_nShownMapID, g_nShownX, g_nShownY);
    }
}


// =====================================================================================================
// Hooks
// =====================================================================================================

// Same test as CWorldMapDlg::CheckSpotInfo (shared with hyperteleportrock.cpp): first spot with |dx|, |dy| <= markWidth / 3.
int FindWorldMapSpotMapID(char* pDlg, int rx, int ry) {
    char* pSpots = *reinterpret_cast<char**>(pDlg + wm::OFF_SPOTS);
    IWzCanvas** apMark = *reinterpret_cast<IWzCanvas***>(pDlg + wm::OFF_MARK_CANVAS);
    if (!pSpots) {
        return 0;
    }
    int nCount = reinterpret_cast<int*>(pSpots)[-1];
    int cx = rx - wm::ORIGIN_X;
    int cy = ry - wm::ORIGIN_Y;
    for (int i = 0; i < nCount; ++i) {
        char* pSpot = pSpots + i * wm::SPOT_STRIDE;
        int nLimit = wm::FALLBACK_HIT;
        int nMark = *reinterpret_cast<int*>(pSpot + wm::SPOT_MARK);
        if (apMark && nMark >= 0 && apMark[nMark]) {
            nLimit = static_cast<int>(apMark[nMark]->width) / 3;
        }
        if (std::abs(*reinterpret_cast<int*>(pSpot + wm::SPOT_X) - cx) > nLimit ||
            std::abs(*reinterpret_cast<int*>(pSpot + wm::SPOT_Y) - cy) > nLimit) {
            continue;
        }
        int* anMapNo = *reinterpret_cast<int**>(pSpot + wm::SPOT_MAPNO);
        return anMapNo && anMapNo[-1] > 0 ? anMapNo[0] : 0;
    }
    return 0;
}

static int __fastcall CWorldMapDlg__OnMouseMove_hook(IUIMsgHandler* pThis, void* _EDX, int rx, int ry) {
    int nResult = CWorldMapDlg__OnMouseMove(pThis, rx, ry);
    int nMapID = 0;
    try {
        if (get_field()) {
            nMapID = FindWorldMapSpotMapID(reinterpret_cast<char*>(pThis) - 4, rx, ry);
        }
    } catch (...) {
    }
    if (nMapID <= 0) {
        ClearMapInfoToolTip();
        return nResult;
    }
    pThis->ClearToolTip(); // CWorldMapDlg::ClearToolTip 0x009E98B7 — drop the stock spot tooltip
    players::MaybeRequest(nMapID);
    g_nShownMapID = nMapID;
    g_nShownX = pThis->GetAbsLeft() + rx;
    g_nShownY = pThis->GetAbsTop() + ry;
    ShowMapInfoToolTip(g_nShownMapID, g_nShownX, g_nShownY);
    return nResult;
}

static void __fastcall CWorldMapDlg__OnDestroy_hook(void* pThis, void* _EDX) {
    ClearMapInfoToolTip();
    CWorldMapDlg__OnDestroy(pThis);
}

void AttachWorldMapInfoMod() {
    ATTACH_HOOK(CWorldMapDlg__OnMouseMove, CWorldMapDlg__OnMouseMove_hook);
    ATTACH_HOOK(CWorldMapDlg__OnDestroy, CWorldMapDlg__OnDestroy_hook);
}
