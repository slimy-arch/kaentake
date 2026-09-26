#include "pch.h"
#include "constants.h"
#include "hook.h"
#include "wvs/util.h"
#include "ztl/ztl.h"

#include <cstdio>
#include <cwchar>
#include <string>

// ===========================================================================
// Monster Book -- "Found In" rows clickable -> world map at that field,
//                 the row STYLING (spec §6.C, PART 2 of this file),
//                 and the EPISODE stat-line styling (spec §6.H / H3, PART 3).
//
// FULL SPEC: monsterbook-implementation.md sections 2, 6.C and 6.H / H3.
//
// This file owns ALL of CUIMonsterBook::Draw's text restyling -- three 5-byte
// call-site rewrites on the same engine primitive (IWzCanvas::DrawTextA,
// 0x004277AD): the two Found In rows in PART 2 and the Episode stat lines in
// PART 3. They share the font cache and the read/release helpers, so keep new
// Draw-text work here rather than opening a fourth site from another module.
//
// Stock v83 draws the Found In page as dead text. This makes a row click open
// the world map already showing that field (and, when the field is not on the
// world map, an English popup instead of the NPC-flavoured stock one).
//
// Three pieces:
//
//  1. CMonsterBookMan::GetInfo (0x00685B79, __thiscall(out, mobId), ret 8) is
//     detoured only to remember which mob is on screen. NOTE its argument is
//     the CARD id, not the mob id (see MobIdFromCardId below) -- it is
//     translated through Item/Consume/0238.img before being cached. It has
//     exactly ONE call site --
//     0x0086720E, the book's "a mob was selected" setter (which right after it
//     tests the returned record at out+4 and bails at 0x0086721A when NULL) --
//     so the cached id is always the mob the page is currently showing.
//
//  2. CUIMonsterBook::OnMouseButton (0x00862184, IUIMsgHandler vtable 0x00B39FE8 slot 2) is
//     detoured, the original runs FIRST, and a left-button RELEASE over the
//     Found In list is turned into a row index.
//
//  3. The row index is turned into an index into the mob's map list, and the
//     map id is read straight out of String/MonsterBook.img -- see below.
//
// --- geometry, re-derived from the exe (the numbers matter, so here is why) --
//
// CUIMonsterBook is drawn in two functions: 0x00863DF1 paints the window and
// the card grid, 0x00865782 paints the right-hand info page. The info page
// content goes into ONE canvas (`[ebp-0x10]` in 0x00865782) that both the
// Dropping icons and the Found In rows share.
//
//   * Found In branch (right tab == 3) @ 0x00865937: list of PAGES at +0xE34,
//     the visible page is `*(int*)(this+0x5B4)`, rows are drawn at canvas
//     y = 0x2D (45) with a 0x14 (20) px step -- 0x00865969 / 0x00865B6E.
//   * Dropping branch (right tab == 2) @ 0x00865B7A draws its icons at the
//     rects cached at this+0xCBC (stride 0x10), built at 0x00863765 as a 4-col
//     grid at (38, 60) pitch 36.
//   * CUIMonsterBook::OnMouseMove @ 0x008623D0 hit-tests exactly those rects
//     after translating them by (+240, +20) -- 0x008623F2..0x0086241A -- and
//     the card grid's hit test (0x00867B61) does the same with (+40, +25) over
//     its own rects at +0xB2C.
//
// So the info-page canvas sits at (240, 20) in WINDOW coordinates -- which is
// the space rx/ry arrive in (0x00867B61 feeds rx/ry straight into PtInRect
// against a window-space rect). Found In row r therefore covers
//     ry in [20 + 45 + 20*r, 20 + 45 + 20*(r+1))  ->  row = (ry - 65) / 20.
// NOT (ry - 45) / 20: that is the CANVAS-space origin, 20 px above the window
// one. The x band keeps clicks off the card grid (window x 48..213), which
// would otherwise open a map every time a card is picked.
//
// --- row -> map index ------------------------------------------------------
//
// The Found In list is NOT one row per map. The builder (0x00867268, the same
// setter that calls GetInfo) walks the record's map array and emits, for each
// map, a STREET HEADER row first whenever the street name differs from the
// previous row's (item+0x0C = 1, drawn at x=30) and then the map row itself
// (item+0x0C = 0, drawn at x=32) -- 0x0086732F..0x00867374, where the map index
// `edi` only advances on the flag==0 row. Pages hold at most 11 rows and a
// header is never left dangling in the last slot (0x0086736B).
//
// So `row + firstVisible` is meaningless here. What IS exact, and needs no
// street-name matching, is:
//
//     mapIndex = number of flag==0 rows strictly before the clicked row,
//                counted over pages 0..selectedPage.
//
// A header row resolves to the same map as the row under it (it did not
// consume a map), so clicking either opens that map -- which is what a user
// expects.
//
// --- map index -> map id ---------------------------------------------------
//
// Read back out of the WZ rather than out of the row record: LoadBook fills the
// record's map array at record+0x0C by walking String/MonsterBook.img/<mob>/map
// index by index (0x006851C5..0x00685253 -- _itoa(i) node name, int converter
// 0x00414D40, ZArray append, no filtering and no sorting), so
// `String/MonsterBook.img/<mob>/map/<index>` IS `record->map[index]`. The row
// record itself only carries the two display strings and the header flag.
// ===========================================================================

namespace {

constexpr uintptr_t kAddrGetInfo = 0x00685B79;              // CMonsterBookMan::GetInfo(out, mobId)
constexpr uintptr_t kAddrOnMouseButton = 0x00862184;        // CUIMonsterBook::OnMouseButton
constexpr uintptr_t kAddrShowFieldOnWorldMap = 0x0087E809;  // __stdcall(int fieldId), ret 4
constexpr uintptr_t kAddrStringPoolGetInstance = 0x0079E805;
constexpr uintptr_t kAddrStringPoolGetString = 0x00406455;  // __thiscall(ZXString<char>* out, unsigned)

// CUIMonsterBook members, offsets from the PRIMARY `this` (= the ecx the Draw at
// 0x00865782 receives; OnMouseButton's ecx is the IUIMsgHandler sub-object, this+4).
constexpr int kMsgHandlerThisDelta = 4;
constexpr int kOffSelectedPage = 0x5B4;   // page index into the Found In / Dropping page array
constexpr int kOffLeftTabCtrl = 0xAD8;    // card colour group, 0..8 (9 = the special non-book page)
constexpr int kOffRightTabCtrl = 0xAE0;   // 0 Basic Info, 1 Episode, 2 Dropping, 3 Found In
constexpr int kOffTabSelected = 0x34;     // selected index inside a tab control
constexpr int kOffFoundInPages = 0xE34;   // ZArray<ZArray<{?, item*}>>
constexpr int kOffRowStreetFlag = 0x0C;   // item+0x0C: != 0 -> street header row

constexpr int kRightTabFoundIn = 3;
// Draw @ 0x0086590E diverts to a completely different info page when the LEFT tab is 9,
// so the Found In rows are not on screen even though the right tab still reads 3.
constexpr int kLeftTabNotABook = 9;

// Window-space geometry of the Found In list (derivation in the header comment).
constexpr int kInfoPageOriginY = 20;
constexpr int kFirstRowCanvasY = 45;
constexpr int kRowHeight = 20;
constexpr int kFirstRowWindowY = kInfoPageOriginY + kFirstRowCanvasY; // 65
constexpr int kInfoPageLeft = 240;
constexpr int kInfoPageRight = 470;

// Sanity ceilings for the page walk. The builder caps a page at 11 rows and the
// fullest mob in String/MonsterBook.img lists 26 maps, so anything far beyond this
// means we are not looking at the structure we think we are -- and it also bounds
// the walk, which runs on the click path.
constexpr int kMaxPages = 256;
constexpr int kMaxRowsPerPage = 64;

// StringPool[0x10E] is shared with the NPC "location" feature, so it is swapped
// only around the call and put back immediately afterwards.
constexpr int kSpNotOnWorldMap = 0x10E;
constexpr char kFoundInNotOnWorldMap[] = "This map is not available on the World Map.";

using GetInfoFn = void*(__thiscall*)(void* pThis, void* pOut, int nMobId);
using OnMouseButtonFn = void(__thiscall*)(void* pThis, unsigned msg, unsigned wParam, int rx, int ry);
using ShowFieldOnWorldMapFn = void(__stdcall*)(int nFieldId);

auto CMonsterBookMan__GetInfo_Orig = reinterpret_cast<GetInfoFn>(kAddrGetInfo);
auto CUIMonsterBook__OnMouseButton_Orig = reinterpret_cast<OnMouseButtonFn>(kAddrOnMouseButton);

// The mob the info page is currently showing (see GetInfo above). 0 = nothing selected yet.
int g_selectedMobId = 0;

// ShowFieldOnWorldMap opens the world map MODALLY (CWnd::DoModal @ 0x004EDBA1 on the
// stack CWorldMapDlg it just created), so it does not return until the player closes
// the map. Guard against a second click sneaking in through that nested message pump.
bool g_showingWorldMap = false;

// ---------------------------------------------------------------------------
// SEH-guarded page/row walk. Its own function with no C++ objects on purpose:
// MSVC rejects __try in a frame that also needs unwinding (C2712), and a hit in
// here must degrade to "do nothing" instead of closing the client for everyone.
// Returns the index into the mob's map list, or -1 when anything looks wrong.
// ---------------------------------------------------------------------------
int ResolveMapIndexFromRow(const void* pBook, int nRow) {
    __try {
        if (!pBook || nRow < 0) {
            return -1;
        }

        const char* base = static_cast<const char*>(pBook);
        const int nSelectedPage = *reinterpret_cast<const int*>(base + kOffSelectedPage);
        const int* const* apPages =
                *reinterpret_cast<const int* const* const*>(base + kOffFoundInPages);
        if (!apPages) {
            return -1;
        }

        const int nPages = *(reinterpret_cast<const int*>(apPages) - 1);
        if (nPages <= 0 || nPages > kMaxPages || nSelectedPage < 0 || nSelectedPage >= nPages) {
            return -1;
        }

        int nMapIndex = 0;
        for (int nPage = 0; nPage <= nSelectedPage; ++nPage) {
            const int* aEntries = apPages[nPage];
            if (!aEntries) {
                return -1;
            }

            const int nRows = *(aEntries - 1);
            if (nRows < 0 || nRows > kMaxRowsPerPage) {
                return -1;
            }

            // Rows to count on this page: all of them for a page we have scrolled
            // past, only the ones above the click on the visible page.
            int nCount = nRows;
            if (nPage == nSelectedPage) {
                if (nRow >= nRows) {
                    return -1; // clicked below the last row -- not a row at all
                }
                nCount = nRow;
            }

            for (int i = 0; i < nCount; ++i) {
                // entry stride 8, the item pointer lives at +4 (Draw: lea eax,[eax+ecx*8] / mov eax,[eax+4]).
                const int* pItem = reinterpret_cast<const int*>(aEntries[i * 2 + 1]);
                if (!pItem) {
                    return -1;
                }
                if (pItem[kOffRowStreetFlag / 4] == 0) {
                    ++nMapIndex; // a real map row consumed one entry of the map list
                }
            }
        }

        return nMapIndex;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Selected index of one of the two tab controls, or -1. Same rule as above: its own
// SEH-only function, because the caller carries a MUSH_FEATURE breadcrumb object and
// MSVC will not mix __try with a frame that needs unwinding.
int ReadTabIndex(const void* pBook, int nTabCtrlOffset) {
    __try {
        if (!pBook) {
            return -1;
        }
        const void* pTab = *reinterpret_cast<const void* const*>(
                static_cast<const char*>(pBook) + nTabCtrlOffset);
        if (!pTab) {
            return -1;
        }
        return *reinterpret_cast<const int*>(static_cast<const char*>(pTab) + kOffTabSelected);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// ---------------------------------------------------------------------------
// String/MonsterBook.img/<mobId>/map/<index>. Node names are the raw, unpadded
// mob id and a plain decimal index (that is how tools/monsterbook_gen writes it
// and how LoadBook reads it back). Non-throwing probe: a missing node comes back
// VT_EMPTY, never as a _com_error.
// ---------------------------------------------------------------------------
int ReadFoundInMapId(int nMobId, int nIndex) {
    if (nMobId <= 0 || nIndex < 0) {
        return 0;
    }

    try {
        if (!get_rm()) {
            return 0;
        }

        wchar_t sPath[96];
        _snwprintf_s(sPath, _countof(sPath), _TRUNCATE, L"String/MonsterBook.img/%d/map/%d", nMobId,
                nIndex);

        Ztl_variant_t vMapId = get_object_or_empty(sPath);
        return get_int32(vMapId, 0);
    } catch (...) {
        return 0;
    }
}

// ---------------------------------------------------------------------------
// GetInfo's argument is the CARD id, NOT the mob id.
//
// CUIMonsterBook::SetMobInfo hands it record+0 (0x00867202), and LoadCardList
// writes the Item/Consume/0238.img NODE NAME -- the card id -- to record+0
// (0x00684B55), while info/mob goes to record+4 (0x00684BA2). GetInfo re-keys
// internally: card map at CMonsterBookMan+0x28 -> record+4 -> book map at +0x88.
//
// String/MonsterBook.img is keyed by MOB id, so the cached value has to be
// translated or every lookup silently misses. Go through the same WZ node the
// loader itself read, rather than reaching into CMonsterBookMan's maps.
// ---------------------------------------------------------------------------
int MobIdFromCardId(int nCardId) {
    if (nCardId <= 0) {
        return 0;
    }

    try {
        if (!get_rm()) {
            return 0;
        }

        wchar_t sPath[96];
        _snwprintf_s(sPath, _countof(sPath), _TRUNCATE, L"Item/Consume/0238.img/%08d/info/mob",
                nCardId);

        return get_int32(get_object_or_empty(sPath), 0);
    } catch (...) {
        return 0;
    }
}

// ---------------------------------------------------------------------------
// StringPool[0x10E] reads "This NPC is located at a spot not available through the
// World Map." -- the wrong words for a Found In click -- so it is swapped only for the
// length of one ShowFieldOnWorldMap call.
//
// StringPool CACHES decoded slots: the getter (0x0079E993) keeps one ZXString<char>
// object per slot at pool->[0][idx], filled once through ZXString::Assign (0x00414617,
// __thiscall(const char*, int), ret 8) and only reads ms_aString on the first fetch.
// Re-pointing ms_aString (the reference module's approach) is therefore invisible once
// the slot has been read. Instead: fetch once to make sure the cache entry exists, then
// Assign our text into that entry and Assign the saved stock text back afterwards.
// Assign reallocates through the engine's own allocator; any copy handed out earlier
// keeps its own reference to the old buffer.
// ---------------------------------------------------------------------------
using ZXStringAssignFn = void(__thiscall*)(void* pStr, const char* s, int n);
constexpr uintptr_t kAddrZXStringAssign = 0x00414617;

ZXString<char>* CachedStringPoolEntry(int nIdx) {
    void* pPool = reinterpret_cast<void*(__cdecl*)()>(kAddrStringPoolGetInstance)();
    if (!pPool) {
        return nullptr;
    }
    ZXString<char> sWarm; // fills the cache slot if nobody has read it yet
    reinterpret_cast<void*(__thiscall*)(void*, ZXString<char>*, unsigned)>(kAddrStringPoolGetString)(
            pPool, &sWarm, static_cast<unsigned>(nIdx));
    auto* aCache = *reinterpret_cast<ZXString<char>***>(pPool);
    return aCache ? aCache[nIdx] : nullptr;
}

class ScopedNotOnWorldMapText {
public:
    ScopedNotOnWorldMapText() {
        try {
            m_pEntry = CachedStringPoolEntry(kSpNotOnWorldMap);
            if (m_pEntry && static_cast<const char*>(*m_pEntry)) {
                m_stock.assign(static_cast<const char*>(*m_pEntry));
                reinterpret_cast<ZXStringAssignFn>(kAddrZXStringAssign)(m_pEntry, kFoundInNotOnWorldMap, -1);
            } else {
                m_pEntry = nullptr;
            }
        } catch (...) {
            m_pEntry = nullptr;
        }
    }
    ~ScopedNotOnWorldMapText() {
        if (m_pEntry) {
            try {
                reinterpret_cast<ZXStringAssignFn>(kAddrZXStringAssign)(m_pEntry, m_stock.c_str(), -1);
            } catch (...) {
            }
        }
    }
    ScopedNotOnWorldMapText(const ScopedNotOnWorldMapText&) = delete;
    ScopedNotOnWorldMapText& operator=(const ScopedNotOnWorldMapText&) = delete;

private:
    ZXString<char>* m_pEntry = nullptr;
    std::string m_stock;
};

void OpenWorldMapAtField(int nMapId) {
    if (nMapId <= 0 || g_showingWorldMap) {
        return;
    }

    g_showingWorldMap = true;
    try {
        ScopedNotOnWorldMapText swap;
        // Opens the dialog, or shows StringPool[0x10E] itself when the field is not
        // on any world-map page (0x0087E839 / 0x0087E854). Blocks until closed.
        reinterpret_cast<ShowFieldOnWorldMapFn>(kAddrShowFieldOnWorldMap)(nMapId);
    } catch (...) {
    }
    g_showingWorldMap = false;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
void* __fastcall CMonsterBookMan__GetInfo_hook(void* pThis, void* /*edx*/, void* pOut, int nMobId) {
    MUSH_FEATURE("monsterBookFoundIn:getInfo");
    // nMobId is really the CARD id -- see MobIdFromCardId. Translate before caching so the
    // String/MonsterBook.img lookups downstream are keyed correctly.
    const int nRealMobId = MobIdFromCardId(nMobId);
    if (nRealMobId > 0) {
        g_selectedMobId = nRealMobId;
    }
    return CMonsterBookMan__GetInfo_Orig(pThis, pOut, nMobId);
}

void __fastcall CUIMonsterBook__OnMouseButton_hook(
        void* pMsgHandler, void* /*edx*/, unsigned msg, unsigned wParam, int rx, int ry) {
    MUSH_FEATURE("monsterBookFoundIn:mouse");

    CUIMonsterBook__OnMouseButton_Orig(pMsgHandler, msg, wParam, rx, ry);

    if (!pMsgHandler || msg != WM_LBUTTONUP || g_selectedMobId <= 0 || g_showingWorldMap) {
        return;
    }
    if (rx < kInfoPageLeft || rx > kInfoPageRight || ry < kFirstRowWindowY) {
        return;
    }

    void* pBook = static_cast<char*>(pMsgHandler) - kMsgHandlerThisDelta;

    // Right tab must be Found In, and the left tab must be a real card group --
    // both are exactly the tests Draw makes before it paints these rows.
    if (ReadTabIndex(pBook, kOffRightTabCtrl) != kRightTabFoundIn) {
        return;
    }
    const int nLeftTab = ReadTabIndex(pBook, kOffLeftTabCtrl);
    if (nLeftTab < 0 || nLeftTab == kLeftTabNotABook) {
        return;
    }

    const int nRow = (ry - kFirstRowWindowY) / kRowHeight;
    const int nMapIndex = ResolveMapIndexFromRow(pBook, nRow);
    if (nMapIndex < 0) {
        return;
    }

    const int nMapId = ReadFoundInMapId(g_selectedMobId, nMapIndex);
    if (nMapId <= 0) {
        return;
    }

    OpenWorldMapAtField(nMapId);
}

// ===========================================================================
// PART 2 -- how the Found In rows are DRAWN (spec §6.C).
//
// Only the painting changes here; the click path above is untouched.
//
//   * street / region header rows (item+0x0C != 0, x=30)  -> RED
//   * map name rows              (item+0x0C == 0, x=32)  -> BLUE
//   * the "(10103040)" the map rows carry is dropped
//   * one size smaller than stock, so a long name stops running off the page
//
// --- what the stock code does ----------------------------------------------
//
// Both rows are painted by the SAME ubiquitous engine helper. Re-derived from
// the exe:
//
//   0x004277AD = IWzCanvas::DrawTextA(int nLeft, int nTop, Ztl_bstr_t sText,
//                                     IWzFont* pFont,
//                                     const Ztl_variant_t& vAlpha,
//                                     const Ztl_variant_t& vTabOrg)
//   __thiscall, `ret 0x18` (6 stack dwords), returns the height the engine
//   reports. It forwards to IWzCanvas::raw_DrawText through vtable +0x98
//   (0x004277F6) and, on the way out, releases the by-value Ztl_bstr_t itself
//   (0x0042780C: `if (arg3) Data_t::Release(arg3)` -> 0x00402EA5). 745 call
//   sites in the exe -- this is THE text primitive, so it is never detoured;
//   only the two Found In CALL SITES are redirected.
//
// Inside CUIMonsterBook::Draw's Found In branch (0x00865937):
//
//   0x008659A0  cmp [item+0x0C], 0 / je 0x00865A7C   <- the header/map split
//   header row  0x008659AE..0x00865A3C : get_basic_font(&f, 0x25) = Arial 12
//               BOLD 0xFF000000 (font factory 0x0098A707, jump table
//               0x0098CB81, case 0x25 at 0x0098BF0F: name StringPool[0x1597]
//               "Arial", size 0xC, colour 0xFF000000, style StringPool[0x584]
//               "B"), text = item+0x10 (streetName), x = 0x1E (30).
//               CALL SITE = 0x00865A3C.
//   map row     0x00865A7C..0x00865B0E : get_basic_font(&f, 1) = the same
//               Arial 12 0xFF000000 without the bold style (case 1 at
//               0x0098A7DF), text = item+0x14 (mapName), x = 0x20 (32).
//               CALL SITE = 0x00865B0E.
//
// y is [ebp-0x18] (45, +20 per row) and the page canvas is the 220x290 one
// Draw creates per call (raw_Create(0xDC, 0x122) @0x00865849).
//
// --- why a call-site redirect and not a Draw detour -------------------------
//
// monsterBookDrops.cpp already owns the ONE detour on CUIMonsterBook::Draw
// (0x00865782) and a second Detours attach on the same target is not worth the
// risk. A post-Draw repaint would also have to erase the stock rows first,
// which means knowing the page background -- whereas swapping the two call
// sites reaches exactly the two DrawTextA invocations that draw these rows and
// nothing else in the client. Detours' trampoline for Draw covers only the
// bytes at its entry, so these two 5-byte patches deep inside the body cannot
// overlap it (nor monsterBook.cpp's branch patches at 0x00864959 / 0x00864E5A /
// 0x00865F9C / 0x00866551, nor monsterBookSearch.cpp's 0x00862009 vtable work).
//
// --- the map id in parentheses ---------------------------------------------
//
// It is NOT ours and it is not in the WZ: CUIMonsterBook::SetMobInfo builds the
// two row strings through CItemInfo::GetMapString (0x005CF792) with the keys
// StringPool[0x6C1] "streetName" (-> item+0x10, 0x0086728D..0x008672BF) and
// StringPool[0x6C2] "mapName" (-> item+0x14, 0x008672DF..0x00867312), and
// features/ui/showIds.cpp detours exactly that function and appends " (%d)"
// whenever the key is "mapName" (USE_SHOW_IDS is TRUE). The clean fix belongs
// there -- see the note in AttachMonsterBookFoundInMod() -- but that file is
// off-limits to this module, so the id is dropped here instead, in OUR buffer,
// without touching the client's cached string.
//
// --- ownership -------------------------------------------------------------
//
// The by-value Ztl_bstr_t (a bare `Data_t*`; Data_t = {BSTR m_wstr, char*
// m_str, ULONG m_RefCount}) is the callee's to release. So exactly one of two
// things happens on every call:
//   * we draw the row ourselves  -> WE release it (0x00402EA5, NULL-guarded
//     exactly like the original at 0x0042780C), or
//   * anything at all goes wrong -> we call the ORIGINAL DrawTextA through and
//     it releases it, and the row simply looks like stock.
// Never both, never neither.
// ===========================================================================

// IWzCanvas::DrawTextA -- see above. NOT detoured; called through on the
// degrade path and reached by the two patched call sites.
constexpr uintptr_t kAddrDrawTextA = 0x004277AD;
// Ztl_bstr_t::Data_t::Release, __thiscall(Data_t*), plain `ret` (0x00402ED5).
// Decrements Data_t+8 and frees through the client's own ZAllocEx pool
// (0x00402EBD / 0x004031ED). MUST NOT be called with NULL: it dereferences
// this+8 before any check.
constexpr uintptr_t kAddrBstrDataRelease = 0x00402EA5;

// The two DrawTextA call sites inside CUIMonsterBook::Draw's Found In branch.
constexpr uintptr_t kCallSiteStreetRow = 0x00865A3C; // item+0x0C != 0, x=30
constexpr uintptr_t kCallSiteMapRow = 0x00865B0E;    // item+0x0C == 0, x=32

// The page canvas Draw creates per call: raw_Create(0xDC, 0x122) @0x00865849.
constexpr int kPageCanvasWidth = 0xDC; // 220
constexpr int kPageRightMargin = 4;

// Stock is Arial 12 in opaque black for both rows (bold for the header). Keep
// the family and the bold-ness, change the colour and drop one pixel of size.
constexpr wchar_t kRowFontFamily[] = L"Arial"; // == StringPool[0x1597]
constexpr wchar_t kRowFontBold[] = L"B";       // == StringPool[0x584]
constexpr unsigned kRowFontHeight = 11;        // stock 12
constexpr unsigned kRowFontHeightSmall = 10;   // only when 11 would not fit
constexpr unsigned kRowColorStreet = 0xFFCC0000; // red   -- region / street header
constexpr unsigned kRowColorMap = 0xFF0000CC;    // blue  -- map name

// Room for the longest name in String/MonsterBook.img plus the id suffix.
constexpr size_t kRowTextMax = 192;

using DrawTextAFn = unsigned(__fastcall*)(void* pCanvas, void* edx, int nLeft, int nTop,
        void* pTextData, void* pFont, const Ztl_variant_t* pvAlpha,
        const Ztl_variant_t* pvTabOrg);
using BstrDataReleaseFn = void(__thiscall*)(void* pData);

auto IWzCanvas__DrawTextA = reinterpret_cast<DrawTextAFn>(kAddrDrawTextA);

// [street?][small?]. Created lazily, once per session per slot; a slot that
// fails to build is never retried (a retry loop on the draw path is worse than
// no restyle at all -- and "no restyle" degrades to the stock look, not to a
// blank row, because the caller falls back to the original DrawTextA).
IWzFontPtr g_rowFont[2][2];
bool g_rowFontTried[2][2] = {};

// ---------------------------------------------------------------------------
// SEH-only helpers. Same rule as ResolveMapIndexFromRow above: MSVC rejects
// __try in a frame that also needs C++ unwinding, and every caller below holds
// smart pointers / Ztl_bstr_t temporaries.
// ---------------------------------------------------------------------------

// Ztl_bstr_t::Data_t::m_wstr, at Data_t+0 -- the very field the stock helper
// hands to raw_DrawText (0x004277BC: `eax = *(void**)arg3`). Read-only.
bool ReadRowText(const void* pTextData, wchar_t* pOut, size_t cchOut) {
    __try {
        if (!pTextData || !pOut || cchOut < 2) {
            return false;
        }
        const wchar_t* pSrc = *reinterpret_cast<const wchar_t* const*>(pTextData);
        if (!pSrc) {
            return false;
        }
        size_t i = 0;
        while (i + 1 < cchOut && pSrc[i]) {
            pOut[i] = pSrc[i];
            ++i;
        }
        pOut[i] = 0;
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (pOut && cchOut) {
            pOut[0] = 0;
        }
        return false;
    }
}

void ReleaseRowText(void* pTextData) {
    __try {
        if (pTextData) {
            reinterpret_cast<BstrDataReleaseFn>(kAddrBstrDataRelease)(pTextData);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---------------------------------------------------------------------------
// "East Rocky Mountain III (10103040)" -> "East Rocky Mountain III".
//
// Deliberately narrow: the tail must be exactly ` (` + one-or-more digits +
// `)`, so a real map name that ends in a parenthesis ("Sleepywood (Free
// Market)") is left alone. Operates on OUR copy only.
// ---------------------------------------------------------------------------
void StripTrailingMapId(wchar_t* s) {
    if (!s) {
        return;
    }
    const size_t n = wcslen(s);
    if (n < 4 || s[n - 1] != L')') {
        return;
    }
    size_t i = n - 2;
    size_t nDigits = 0;
    while (i > 0 && s[i] >= L'0' && s[i] <= L'9') {
        --i;
        ++nDigits;
    }
    if (nDigits == 0 || i == 0 || s[i] != L'(' || s[i - 1] != L' ') {
        return;
    }
    s[i - 1] = 0;
}

// Shared by the Found In rows and the Episode stat lines below. A null return is the caller's
// cue to fall back to the stock DrawTextA -- never to draw nothing.
IWzFontPtr CreateArialFont(unsigned uHeight, unsigned uColor, bool bBold) {
    IWzFontPtr pFont;
    try {
        PcCreateObject<IWzFontPtr>(L"Canvas#Font", pFont, nullptr);
        if (!pFont) {
            return pFont;
        }
        Ztl_bstr_t sName(kRowFontFamily);
        Ztl_variant_t vStyle; // VT_EMPTY == regular, exactly like basic font index 1
        if (bBold) {
            vStyle = kRowFontBold; // basic font index 0x25 is bold; keep the hierarchy
        }
        if (FAILED(pFont->raw_Create(sName, uHeight, uColor, vStyle))) {
            pFont = nullptr;
        }
    } catch (...) {
        pFont = nullptr;
    }
    return pFont;
}

IWzFont* EnsureRowFont(bool bStreet, bool bSmall) {
    const int r = bStreet ? 1 : 0;
    const int c = bSmall ? 1 : 0;
    if (g_rowFont[r][c]) {
        return g_rowFont[r][c];
    }
    if (g_rowFontTried[r][c]) {
        return nullptr;
    }
    g_rowFontTried[r][c] = true;

    g_rowFont[r][c] = CreateArialFont(bSmall ? kRowFontHeightSmall : kRowFontHeight,
            bStreet ? kRowColorStreet : kRowColorMap, bStreet);

    if (!g_rowFont[r][c]) {
        DEBUG_MESSAGE("[monsterBookFoundIn] row font (street=%d small=%d) failed -- stock look\n",
                bStreet ? 1 : 0, bSmall ? 1 : 0);
    }
    return g_rowFont[r][c];
}

// "Do we still overflow?" -- anything we cannot measure answers "it fits", so a
// measuring failure can never shrink a row that did not need shrinking.
bool RowTextFits(IWzFont* pFont, const wchar_t* sText, int nAvail) {
    if (!pFont || !sText || nAvail <= 0) {
        return true;
    }
    try {
        unsigned uWidth = 0;
        Ztl_bstr_t s(sText);
        if (FAILED(pFont->raw_CalcTextWidth(s, vtEmpty, &uWidth))) {
            return true;
        }
        if (uWidth == 0 || uWidth > 4096) {
            return true;
        }
        return static_cast<int>(uWidth) <= nAvail;
    } catch (...) {
        return true;
    }
}

unsigned DrawFoundInRow(void* pCanvas, int nLeft, int nTop, void* pTextData, void* pFont,
        const Ztl_variant_t* pvAlpha, const Ztl_variant_t* pvTabOrg, bool bStreet) {
    // Degrade path: hand the call to the stock helper untouched. It draws with
    // the stock font AND releases pTextData, so this branch must not release.
    wchar_t sText[kRowTextMax];
    if (!pCanvas || !ReadRowText(pTextData, sText, kRowTextMax)) {
        return IWzCanvas__DrawTextA(
                pCanvas, nullptr, nLeft, nTop, pTextData, pFont, pvAlpha, pvTabOrg);
    }

    if (!bStreet) {
        StripTrailingMapId(sText); // only "mapName" ever carries the showIds suffix
    }

    IWzFont* pOurFont = EnsureRowFont(bStreet, false);
    if (pOurFont) {
        const int nAvail = kPageCanvasWidth - nLeft - kPageRightMargin;
        if (!RowTextFits(pOurFont, sText, nAvail)) {
            IWzFont* pSmaller = EnsureRowFont(bStreet, true);
            if (pSmaller) {
                pOurFont = pSmaller;
            }
        }
    }
    if (!pOurFont) {
        return IWzCanvas__DrawTextA(
                pCanvas, nullptr, nLeft, nTop, pTextData, pFont, pvAlpha, pvTabOrg);
    }

    unsigned uHeight = 0;
    try {
        // raw_*, not the throwing DrawTextA wrapper: a failed HRESULT inside a
        // draw path must be a no-op, not a C++ throw through the engine.
        // vAlpha / vTabOrg are passed straight through as the client built them.
        const Ztl_variant_t& vAlpha = pvAlpha ? *pvAlpha : vtEmpty;
        const Ztl_variant_t& vTabOrg = pvTabOrg ? *pvTabOrg : vtEmpty;
        Ztl_bstr_t s(sText);
        static_cast<IWzCanvas*>(pCanvas)->raw_DrawText(
                nLeft, nTop, s, pOurFont, vAlpha, vTabOrg, &uHeight);
    } catch (...) {
        // a COM/allocation throw here must never reach the engine's draw loop
    }

    ReleaseRowText(pTextData); // we did not call through -> the release is ours
    return uHeight;
}

unsigned DrawFoundInRowGuarded(void* pCanvas, int nLeft, int nTop, void* pTextData, void* pFont,
        const Ztl_variant_t* pvAlpha, const Ztl_variant_t* pvTabOrg, bool bStreet) {
    __try {
        return DrawFoundInRow(pCanvas, nLeft, nTop, pTextData, pFont, pvAlpha, pvTabOrg, bStreet);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The two replacements. __fastcall(ecx, edx, 6 stack dwords) reproduces the
// __thiscall / `ret 0x18` frame of 0x004277AD exactly, so the patched call
// sites stay stack-balanced. One function per site: the header/map decision
// then comes from WHICH branch of Draw we are in, never from guessing at x.
unsigned __fastcall CUIMonsterBook_DrawStreetRow_hook(void* pCanvas, void* /*edx*/, int nLeft,
        int nTop, void* pTextData, void* pFont, const Ztl_variant_t* pvAlpha,
        const Ztl_variant_t* pvTabOrg) {
    MUSH_FEATURE("monsterBookFoundIn:streetRow");
    return DrawFoundInRowGuarded(pCanvas, nLeft, nTop, pTextData, pFont, pvAlpha, pvTabOrg, true);
}

unsigned __fastcall CUIMonsterBook_DrawMapRow_hook(void* pCanvas, void* /*edx*/, int nLeft,
        int nTop, void* pTextData, void* pFont, const Ztl_variant_t* pvAlpha,
        const Ztl_variant_t* pvTabOrg) {
    MUSH_FEATURE("monsterBookFoundIn:mapRow");
    return DrawFoundInRowGuarded(pCanvas, nLeft, nTop, pTextData, pFont, pvAlpha, pvTabOrg, false);
}

// ===========================================================================
// PART 3 -- the EPISODE tab's stat lines (spec §6.H / H3, user point 11).
//
// The data pipeline (Cosmic tools/monsterbook_data/build_book.py) no longer
// bakes a lore paragraph into String/MonsterBook.img/<mob>/episode. It bakes
// the mob's WZ combat stats instead, one per line:
//
//     Lv. : 1                <- header block, drawn STOCK
//     Form : Mammal          <- header block, drawn STOCK
//                            <- blank
//     PAD : 12               <- category BLUE, value RED
//     MAD : 0
//     ...
//     ELEM : Fire weak
//
// --- how the Episode page is drawn (all re-derived from the exe) -----------
//
// CUIMonsterBook::Draw's Episode branch (right tab == 1) is at 0x00865DC7. It
// is shaped exactly like the Found In branch: a PAGE array at this+0xE30, the
// visible page `*(int*)(this+0x5B4)` (0x00865DDE), entries of stride 8 with
// the item pointer at +4 (0x00865E10). Each item carries a TYPE at +0
// (0x00865E21..0x00865E34):
//
//     type 0 -> a text run   -> 0x00865EC0, DrawTextA at *** 0x00865F47 ***
//     type 1 -> an image run -> 0x00865E3A, canvas vtable +0x80
//     other  -> skipped      -> 0x00865F82
//
// The type-0 draw reads x from item+0x18 (0x00865F3A), y from item+0x1C + 45
// (0x00865F2B), the text (a Ztl_bstr_t) from item+0x10 (0x00865F0E) and the
// font (an IWzFontPtr) from item+0x0C (0x00865EFB) -- so the call site takes
// exactly the same six arguments as the two Found In ones and can be swapped
// the same way.
//
// --- is a record one LINE, or one word? (this is the whole question) -------
//
// One line. The records are produced by the client's generic rich-text layout
// (0x0099DD54, reached from CUIMonsterBook::SetMobInfo at 0x008675A3 with the
// episode string, a wrap width of 0xDC = 220 px stamped at 0x00867550). Its
// tokeniser (0x009A264F -> 0x009A2A2C) ACCUMULATES characters into one token
// until it hits `\0`, `\r` (0x0D), `#` (0x23) or `\` (0x5C) -- 0x009A2A5B..
// 0x009A2A6E -- and the plain-text case then writes the whole token to
// record+0x10 with type 0 (0x009A205F: text to +0x10, `mov [ebx], esi` with
// esi == 0 to +0). `\` + the next char is consumed as ONE line break
// (0x009A2746 re-enters the scanner with the consume flag set), which is why
// the WZ stores line breaks as the two characters `\` `n`.
//
// So a baked `PAD : 12` arrives here as ONE record holding the whole string,
// and it is far short of the 220 px at which the layout would split it
// (0x009A21E6). That is also why the data half emits one short `ELEM : ...`
// line per element pair instead of one packed line.
//
// --- what this does --------------------------------------------------------
//
// Split the record's text on the FIRST " : ", draw the part before it (plus
// the separator) in BLUE, and the rest in RED at blue-width px to its right.
// Everything else -- a line with no " : ", and the `Lv.` / `Form` header,
// which DO contain one -- goes to the stock helper untouched.
//
// The ownership rule is the Found In one, unchanged: the by-value Ztl_bstr_t
// belongs to the callee, so either we call the original through (it releases)
// or we draw it ourselves (we release). Exactly once, never both.
// ===========================================================================

// The single DrawTextA call site inside Draw's Episode branch. Byte-verified
// before it is patched -- see IsStockDrawTextCallSite.
constexpr uintptr_t kCallSiteEpisodeLine = 0x00865F47;

// Same two colours the Found In rows use, so the book reads as one design.
// The value's red is NOT bold (the Found In street header is) -- a stat block
// with every number bold is noise.
constexpr unsigned kEpisodeColorCategory = 0xFF0000CC; // blue -- "PAD : "
constexpr unsigned kEpisodeColorValue = 0xFFCC0000;    // red  -- "12"

// The separator the data half bakes. Three characters, space-colon-space.
constexpr wchar_t kEpisodeSeparator[] = L" : ";
constexpr size_t kEpisodeSeparatorLen = 3;

// Header lines that keep the stock colour. They carry a " : " like every stat
// line does, so they have to be named rather than detected.
constexpr wchar_t kEpisodeHeaderLv[] = L"Lv.";
constexpr wchar_t kEpisodeHeaderLvShort[] = L"Lv";
constexpr wchar_t kEpisodeHeaderForm[] = L"Form";

// [0] category (blue), [1] value (red). Same lazy, once-per-session, never
// retried policy as the Found In fonts: a retry loop on a draw path is worse
// than no restyle, and "no restyle" here means the stock look, not a gap.
IWzFontPtr g_episodeFont[2];
bool g_episodeFontTried[2] = {};

IWzFont* EnsureEpisodeFont(bool bValue) {
    const int i = bValue ? 1 : 0;
    if (g_episodeFont[i]) {
        return g_episodeFont[i];
    }
    if (g_episodeFontTried[i]) {
        return nullptr;
    }
    g_episodeFontTried[i] = true;

    g_episodeFont[i] = CreateArialFont(
            kRowFontHeight, bValue ? kEpisodeColorValue : kEpisodeColorCategory, false);

    if (!g_episodeFont[i]) {
        DEBUG_MESSAGE("[monsterBookFoundIn] episode %s font failed -- stock look\n",
                bValue ? "value" : "category");
    }
    return g_episodeFont[i];
}

// The `Lv.` / `Form` block keeps stock colour (spec §6.H). Compared case
// -insensitively and on the WHOLE category, so a stat that merely starts with
// those letters could never be swallowed by it.
bool IsEpisodeHeaderCategory(const wchar_t* sText, size_t nLen) {
    struct { const wchar_t* s; size_t n; } aHeaders[] = {
        { kEpisodeHeaderLv, _countof(kEpisodeHeaderLv) - 1 },
        { kEpisodeHeaderLvShort, _countof(kEpisodeHeaderLvShort) - 1 },
        { kEpisodeHeaderForm, _countof(kEpisodeHeaderForm) - 1 },
    };
    for (const auto& h : aHeaders) {
        if (nLen == h.n && _wcsnicmp(sText, h.s, h.n) == 0) {
            return true;
        }
    }
    return false;
}

// Width of the blue half, so the red half can start exactly where it ends.
// Unlike RowTextFits above, an unmeasurable string is a HARD failure here: we
// would have no idea where to put the value, and drawing it on top of the
// category is worse than not restyling at all.
bool MeasureEpisodeCategory(IWzFont* pFont, const wchar_t* sText, int& nOutWidth) {
    if (!pFont || !sText) {
        return false;
    }
    try {
        unsigned uWidth = 0;
        Ztl_bstr_t s(sText);
        if (FAILED(pFont->raw_CalcTextWidth(s, vtEmpty, &uWidth))) {
            return false;
        }
        if (uWidth == 0 || uWidth > 4096) {
            return false;
        }
        nOutWidth = static_cast<int>(uWidth);
        return true;
    } catch (...) {
        return false;
    }
}

unsigned DrawEpisodeLine(void* pCanvas, int nLeft, int nTop, void* pTextData, void* pFont,
        const Ztl_variant_t* pvAlpha, const Ztl_variant_t* pvTabOrg) {
    // Every `return DrawStock(...)` below hands the call to the stock helper
    // UNTOUCHED -- it draws with the client's own font and releases pTextData,
    // so none of those paths may release it themselves.
    const auto DrawStock = [&]() -> unsigned {
        return IWzCanvas__DrawTextA(
                pCanvas, nullptr, nLeft, nTop, pTextData, pFont, pvAlpha, pvTabOrg);
    };

    wchar_t sText[kRowTextMax];
    if (!pCanvas || !ReadRowText(pTextData, sText, kRowTextMax)) {
        return DrawStock();
    }

    const wchar_t* pSep = wcsstr(sText, kEpisodeSeparator);
    if (!pSep) {
        return DrawStock(); // a blank line, or anything the pipeline did not shape
    }

    const size_t nCategory = static_cast<size_t>(pSep - sText);
    if (nCategory == 0 || IsEpisodeHeaderCategory(sText, nCategory)) {
        return DrawStock();
    }

    const wchar_t* sValue = pSep + kEpisodeSeparatorLen;
    if (!*sValue) {
        return DrawStock(); // "PAD : " with nothing after it -- nothing to colour
    }

    // Blue half = the category AND the separator, so the red starts on the value.
    const size_t nBlue = nCategory + kEpisodeSeparatorLen;
    if (nBlue >= kRowTextMax) {
        return DrawStock();
    }
    wchar_t sCategory[kRowTextMax];
    wmemcpy(sCategory, sText, nBlue);
    sCategory[nBlue] = 0;

    IWzFont* pCategoryFont = EnsureEpisodeFont(false);
    IWzFont* pValueFont = EnsureEpisodeFont(true);
    if (!pCategoryFont || !pValueFont) {
        return DrawStock();
    }

    // Measure BEFORE drawing anything: once the blue half is on the canvas the
    // stock fallback is no longer available (it would draw the line twice and
    // release the string a second time).
    int nBlueWidth = 0;
    if (!MeasureEpisodeCategory(pCategoryFont, sCategory, nBlueWidth)) {
        return DrawStock();
    }

    unsigned uHeight = 0;
    try {
        // raw_*, not the throwing wrappers: a failed HRESULT inside a draw path
        // must be a no-op, not a C++ throw through the engine's draw loop.
        // vAlpha / vTabOrg are passed straight through as the client built them.
        const Ztl_variant_t& vAlpha = pvAlpha ? *pvAlpha : vtEmpty;
        const Ztl_variant_t& vTabOrg = pvTabOrg ? *pvTabOrg : vtEmpty;
        IWzCanvas* pTarget = static_cast<IWzCanvas*>(pCanvas);

        Ztl_bstr_t sBlue(sCategory);
        pTarget->raw_DrawText(nLeft, nTop, sBlue, pCategoryFont, vAlpha, vTabOrg, &uHeight);

        unsigned uValueHeight = 0;
        Ztl_bstr_t sRed(sValue);
        pTarget->raw_DrawText(nLeft + nBlueWidth, nTop, sRed, pValueFont, vAlpha, vTabOrg,
                &uValueHeight);
        if (uHeight == 0) {
            uHeight = uValueHeight;
        }
    } catch (...) {
        // a COM/allocation throw here must never reach the engine's draw loop
    }

    ReleaseRowText(pTextData); // we did not call through -> the release is ours
    return uHeight;
}

unsigned DrawEpisodeLineGuarded(void* pCanvas, int nLeft, int nTop, void* pTextData, void* pFont,
        const Ztl_variant_t* pvAlpha, const Ztl_variant_t* pvTabOrg) {
    __try {
        return DrawEpisodeLine(pCanvas, nLeft, nTop, pTextData, pFont, pvAlpha, pvTabOrg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Same __fastcall(ecx, edx, 6 stack dwords) shape as the two Found In
// replacements, which reproduces 0x004277AD's __thiscall / `ret 0x18` frame
// exactly, so the patched call site stays stack-balanced.
unsigned __fastcall CUIMonsterBook_DrawEpisodeLine_hook(void* pCanvas, void* /*edx*/, int nLeft,
        int nTop, void* pTextData, void* pFont, const Ztl_variant_t* pvAlpha,
        const Ztl_variant_t* pvTabOrg) {
    MUSH_FEATURE("monsterBookFoundIn:episodeLine");
    return DrawEpisodeLineGuarded(pCanvas, nLeft, nTop, pTextData, pFont, pvAlpha, pvTabOrg);
}

// "Is this still the stock `call 0x004277AD` we reversed?" Cheap insurance
// against patching a byte range some other module (or a different client
// build) already owns -- PatchCall itself writes unconditionally.
bool IsStockDrawTextCallSite(uintptr_t uCallSite) {
    __try {
        const unsigned char* pBytes = reinterpret_cast<const unsigned char*>(uCallSite);
        if (pBytes[0] != 0xE8) {
            return false;
        }
        const intptr_t nRel = *reinterpret_cast<const int*>(uCallSite + 1);
        return static_cast<uintptr_t>(uCallSite + 5 + nRel) == kAddrDrawTextA;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void AttachMonsterBookFoundInMod() {
#if USE_MONSTER_BOOK_FOUNDIN
    ATTACH_HOOK(CMonsterBookMan__GetInfo_Orig, CMonsterBookMan__GetInfo_hook);
    ATTACH_HOOK(CUIMonsterBook__OnMouseButton_Orig, CUIMonsterBook__OnMouseButton_hook);

    // §6.C row styling. Two 5-byte call-site rewrites inside CUIMonsterBook::Draw's
    // Found In branch -- NOT a detour on IWzCanvas::DrawTextA (0x004277AD), which has
    // 745 call sites across the client, and NOT a second detour on Draw itself
    // (0x00865782), which monsterBookDrops.cpp owns. PatchCall has no compiler-visible
    // call site, so both replacements are registered for crash attribution by hand.
    REGISTER_CODECAVE(CastHook(&CUIMonsterBook_DrawStreetRow_hook), "monsterBookFoundIn:streetRow");
    REGISTER_CODECAVE(CastHook(&CUIMonsterBook_DrawMapRow_hook), "monsterBookFoundIn:mapRow");
    PatchCall(kCallSiteStreetRow, CastHook(&CUIMonsterBook_DrawStreetRow_hook));
    PatchCall(kCallSiteMapRow, CastHook(&CUIMonsterBook_DrawMapRow_hook));

    // §6.H / H3 Episode stat lines. A THIRD call-site rewrite, on the one DrawTextA inside
    // Draw's Episode branch (0x00865DC7 -> type-0 record -> 0x00865F47). Byte-verified first,
    // because unlike a Detours attach PatchCall writes whatever is there. A guard failure
    // leaves the Episode tab looking stock -- the baked text still reads correctly, it just
    // stays black -- which is the right way to lose this feature.
    if (IsStockDrawTextCallSite(kCallSiteEpisodeLine)) {
        REGISTER_CODECAVE(
                CastHook(&CUIMonsterBook_DrawEpisodeLine_hook), "monsterBookFoundIn:episodeLine");
        PatchCall(kCallSiteEpisodeLine, CastHook(&CUIMonsterBook_DrawEpisodeLine_hook));
        DEBUG_MESSAGE("[monsterBookFoundIn] episode line restyle installed at 0x%08X\n",
                static_cast<unsigned>(kCallSiteEpisodeLine));
    } else {
        DEBUG_MESSAGE("[monsterBookFoundIn] GUARD FAILED at 0x%08X -- not a stock "
                      "`call 0x004277AD`; episode restyle SKIPPED\n",
                static_cast<unsigned>(kCallSiteEpisodeLine));
    }

    // NOTE for whoever owns features/ui/showIds.cpp: the "(10103040)" on these rows is
    // appended by ITS GetMapString_hook (0x005CF792) whenever sKey == "mapName", and
    // that is the right place to fix it -- the Found In list builder
    // (CUIMonsterBook::SetMobInfo, 0x008672DF) is just one of its callers. The minimal
    // change there is a suppression flag consulted next to the strcmp, raised around
    // the book's list build. Until then this module strips the suffix from its own
    // copy of the string at draw time; nothing the client cached is modified.
#endif
}
