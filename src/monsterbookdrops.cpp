#include "pch.h"
#include "constants.h"
#include "debug.h"
#include "hook.h"
#include "wvs/packet.h" // CInPacket / COutPacket
#include "wvs/util.h"   // get_rm / get_object_or_empty / get_item_or_empty / get_int32
#include "ztl/ztl.h"    // IWz*Ptr / PcCreateObject / Ztl_bstr_t / vtEmpty

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

// ===========================================================================================
// MONSTER BOOK -- drop chance % under each icon of the "Dropping" tab.
// FULL SPEC: monsterbook-implementation.md section 6.B (which supersedes section 3).
//
// The book is 100% client-side, so the ONLY thing the server owns here is the number: the drop
// chance at the asking player's LIVE drop / boss / card rates. That travels over the Kentakae
// pair
//
//     C2S 0x372B MONSTER_BOOK_QUERY  [byte 0][int mobId]
//     S2C 0x372C MONSTER_BOOK_RESULT [byte 0][int mobId][short n][ n x (int itemId, int ppm) ]
//
// with `ppm` in parts per million AFTER the rates, i.e. percent = ppm / 10000.0. The reply is
// cached per mob id and invalidated when the selection changes. Nothing is drawn until it lands
// (no placeholder, no flicker) and nothing is ever sent from a draw or packet path -- the query
// goes out on the main-thread flush like every other Kentakae feature.
//
// ---------------------------------------------------------------------------------------------
// HOW THE STOCK PAGE IS BUILT (all of this is read off the exe, MapleStory.exe @0x00400000)
// ---------------------------------------------------------------------------------------------
// CUIMonsterBook::Draw @0x00865782 (__thiscall, `ret`, no args) renders the RIGHT-HAND PAGE into
// a throwaway 220x290 canvas it creates per call (PcCreateObject("Canvas") + IWzCanvas::Create
// (0xDC, 0x122) @0x00865849) and, at the very end (0x00866A06), blits that canvas onto
//
//     (*(IWzGr2DLayer**)(this + 0xB20))->canvas[0]      via IWzCanvas::Copy(0, 0, page, <empty>)
//
// The layer is built ONCE in OnCreate at window position (240, 20) (0x008633F7 -> 0x00863409),
// so the layer canvas and the page canvas share one coordinate space and the composite fully
// replaces it every Draw. That is exactly what makes a POST-Draw detour the right place to add
// labels: we paint on the same canvas, in the same coordinates the icons were painted in, and
// the next Draw wipes and we repaint. Nothing accumulates.
//
// Draw reads its two tab indices at its head (0x00865792 / 0x0086579E) and dispatches at
// 0x0086590E:  left tab == 9 -> 0x00866961 (special page); else right tab 0 -> Basic Info
// (0x00865F96), 1 -> Episode (+0xE30, 0x00865DC7), 2 -> Dropping (+0xE38, 0x00865B7A),
// 3 -> Found In (+0xE34, 0x00865937).
//
// The Dropping branch is at 0x00865B7A:
//   * pages  = *(void***)(this + 0xE38), page count at pages[-1]
//   * page   = pages[*(int*)(this + 0x5B4)]           <- +0x5B4 is the PAGE index here
//   * slot i = page[2i+1] (stride 8, value at +4), i < page[-1]  (NO hard-coded slot count)
//   * the icon goes at the RECT this + 0xCBC + i*0x10: IconBase blitted at (rc.left, rc.top)
//     (0x00865CF7/0x00865D1B) and the item icon anchored at (rc.left, rc.bottom) at
//     0x00865D59/0x00865D8B -- item icons carry an origin of ~(1,33), so the glyph lands back
//     inside the 32x32 cell. Draw never reads rc.right.
//
// ---------------------------------------------------------------------------------------------
// SECTION 6.B DEFECT 1 -- "the labels only show up on the SECOND visit". ROOT CAUSE.
// ---------------------------------------------------------------------------------------------
// CUIMonsterBook::Draw is NOT called per frame. The window paints through a DIRTY-FLAG pump:
// vtable slot 21 (0x00B3A034 -> 0x00861E5C, __thiscall, no args) is the per-frame Draw override,
// and its whole body is
//
//     for (i = 0; i < 3; ++i)                       // 0x00861E61..0x00861E80
//         if (*(int*)(this + 0xB14 + 8*i)) {        // pane i dirty?
//             Redraw(i);                            // 0x00866A9E: 0 -> 0x00863DF1 (frame+grid),
//             *(int*)(this + 0xB14 + 8*i) = 0;      //             1 -> 0x00865782 (RIGHT page),
//         }                                         //             2 -> 0x00865220
//     CUIWnd::Draw(this);                           // 0x009E067E, the shared base draw
//
// so the right-hand page is repainted ONLY when this+0xB1C is non-zero, and the pump clears it
// immediately. The client raises it on user-driven state changes and nowhere else -- SetPage
// (0x00866AF3: `mov [ecx+0x5B4],eax` + `mov [ecx+0xB1C],1`), SetRightTab (0x00866B0A: also
// +0xB24), the card click at 0x008679C5, 0x008633AE.
//
// The drop table arrives asynchronously, one round trip AFTER the tab switch that painted the
// page and cleared the flag. Nothing marks the page dirty when it lands, so Draw never runs
// again, so the post-Draw label pass never runs -- until the player touches a tab or a page
// arrow, which re-dirties the pane and repaints it with a cache that is warm by then. That is
// exactly the reported "open Dropping, leave, come back" behaviour, and it is a MISSING
// INVALIDATION, not a race and not a timing window.
//
// FIX: detour the pump itself (0x00861E5C) and, when new data has landed since the last paint,
// set this+0xB1C = 1 BEFORE calling through -- the same single store the client's own
// SetPage/SetRightTab perform. The client's own pump then repaints the pane on that very frame.
// No timer, no polling, and Draw is never called by us. The pump detour is also the only place
// where a CUIMonsterBook pointer is guaranteed live (we are inside one of its methods), so no
// pointer is ever cached across the window's lifetime.
//
// ---------------------------------------------------------------------------------------------
// SECTION 6.B DEFECT 2 -- 5 rows -> 4 rows, % below the icon.
// ---------------------------------------------------------------------------------------------
// Two client constants have to move together, and they were both re-read from the exe:
//
//   a) the RECT table, built ONCE from OnCreate (0x00861E99 -> 0x00861EDA -> 0x00863717) at
//      0x0086375F..0x008637A1 as 20 slots:
//          idiv 4 -> left = 36*(i%4) + 38   (`lea ecx,[eax*4+0x26]` after `lea eax,[edx+edx*8]`)
//                    top  = 36*(i/4) + 60   (`lea eax,[eax*4+0x3c]` after `lea eax,[eax+eax*8]`)
//                    right = left+32, bottom = top+32   (`lea edx,[ecx+0x20]`/`[eax+0x20]`)
//      and 0xCBC + 20*0x10 == 0xDFC, the next known member, so the array is exactly 20 entries.
//
//   b) the list builder pages 20 per page: `cmp dword ptr [ebp-0x1C], 0x14` at 0x008674CF
//      (bytes 83 7D E4 14), the counter incremented at 0x008674B3, the finished page pushed
//      into this+0xE38 at 0x008674E2. The Found In builder above it uses different locals
//      (byte offset in [ebp-0x18], caps 0x50/0x58), so this imm8 is the Dropping page size
//      and nothing else.
//
// We do NOT rewrite the builder's arithmetic (it would need a longer instruction encoding).
// We patch (b)'s imm8 20 -> 16 at attach time, after verifying the four opcode bytes, and
// rewrite (a)'s table from the pump detour, before the client reads it:
//
//      left = 36*(i%4) + 38          (unchanged -- the columns are not what 6.B asks to move)
//      top  = 48*(i/4) + 60          (row step 36 -> 48)
//
// 48*3 + 60 + 32 == 236 == the stock 36*4 + 60 + 32, i.e. the grid keeps EXACTLY its old
// vertical extent; the row that went away is spent as 16 px of air between the remaining rows,
// and the % label lives in that air, below the icon and outside it.
//
// Everything downstream adapts on its own: Draw bounds its slot loop by the page's own item
// count (0x00865BBE), and the page arrows are enabled from `pageIndex < pages[-1]-1`
// (0x00863DC5..0x00863DE9), so the page count follows the new per-page size.
//
// If the imm8 signature does not match (a different exe build), the paging patch is skipped AND
// the rect rewrite is skipped with it, so the tab degrades to the stock 4x5 grid instead of
// showing 20 icons in 16 slots.
//
// The only other reader of the rect table is CUIMonsterBook::OnMouseMove (0x008623F2), and it
// is DEAD CODE in this build: it walks the array from `this+0xCB8` (`lea edi,[esi+0xCC0]` with
// reads at [edi-8]..[edi+4]), i.e. one dword BEFORE the array the builder and Draw agree on, so
// every rect it assembles has left > right and PtInRect always fails. Verified with the stock
// numbers (slot 0 -> SetRect(380, 58, 300, 90)). The new geometry does not resurrect it for
// slots inside a row; at the three row boundaries the bogus rect stops being empty, exactly as
// it already does in stock, only 16 px wide instead of 4 -- see the report notes.
//
// ---------------------------------------------------------------------------------------------
// The list itself is built at 0x008673FE from the CMonsterBookMan record's `reward` array
// (record+0x10, filled by LoadBook from String/MonsterBook.img/<mobId>/reward): a straight
// in-order walk, no filtering. So with the paging patch applied
//     slot i of page p  ==  String/MonsterBook.img/<mobId>/reward/<p*16 + i>
// which is how we map an icon to an item id without decoding the row records (their item id
// lives behind a ZtlSecure fuse at row+0xC, decoded by 0x0042873D -- deliberately not touched).
//
// The selected mob comes from CUIMonsterBook::SetMobInfo @0x00866C2C (__thiscall, `ret 8`),
// which takes an 8-byte by-value card holder: arg0 = holder head, arg1 = CardRecord*. The card
// record is {+0 cardId, +4 mobId, +8 tab, +0xC count} -- LoadCardList writes the 0238.img node
// name (the CARD id) to +0 at 0x00684B55 and info/mob to +4 at 0x00684BA2, and SetMobInfo feeds
// +4 to "Mob/%07d.img" at 0x00866CD6. We read +4 BEFORE calling through, because the holder's
// destructor runs inside the original and may release the record.
//
// CMonsterBookMan::GetInfo is deliberately NOT hooked here: monsterBookFoundIn.cpp (spec §2)
// owns that detour, and two Detours attaches on one target is not a thing worth risking.
//
// SHARED RIGHT PAGE (spec §6, shared rule): monsterBookSearch.cpp renders plain item icons on
// this same page while an item search result is up, and those must carry NO percentages. The
// label pass returns early on MonsterBookSearch_IsItemResultView() -- and while it is raised we
// also stop counting the Dropping tab as "on screen", so no drop query is armed for it either.
//
// ---------------------------------------------------------------------------------------------
// SECTION 6.F / F3 (1) -- STALE ICONS. A slot whose item is NOT in the server's reply.
// ---------------------------------------------------------------------------------------------
// The ICON LIST is baked: it comes from String/MonsterBook.img/<mob>/reward, generated offline
// from drop_data (Cosmic tools/regen_monsterbook.ps1). The PERCENTAGES are live: the server
// computes them from drop_data at query time. So the two can disagree in exactly one direction --
// a drop deleted from drop_data (or one the server declines to report) still has a baked icon,
// and until the book data is regenerated that icon sits there advertising a drop that no longer
// exists. Before this change such a slot simply got no label, which reads as "still loading".
//
// Now, and ONLY once a reply for the selected mob has landed (g_chanceMob == g_mobId -- an EMPTY
// reply counts, it is still an answer), such a slot is OVERPAINTED with the page's own empty
// plate, `UI/UIWindow.img/IconBase/0`. That is byte-for-byte the same blit the stock Dropping
// pass performs at 0x00865CF7/0x00865D1B before it stamps the item icon on top, so the slot ends
// up looking exactly like the empty slot the client would draw after the book data is regenerated
// -- no new art, no new geometry, nothing the client does not already do to this canvas.
// If the plate cannot be resolved the slot gets a 50%-alpha grey wash instead (DrawRectangle
// BLENDS src-over on partial alpha -- verified in Canvas.dll, see runeMinigame.cpp), so the icon
// is dimmed rather than left looking live. With NO reply, nothing is touched at all.
//
// ---------------------------------------------------------------------------------------------
// SECTION 6.H / H2 (4) -- THE HOME SCREEN MUST NOT SHOW A MOB'S DATA.
// ---------------------------------------------------------------------------------------------
// Left tab 9 is the book's cover / "Book Master Lvl" page. Draw diverts to it at 0x0086590E
// (`cmp [ebp-0x40], edx` on the `leftTab == 9` flag computed at 0x008657AC) and paints the mandala
// instead of any of the four content pages -- but it does that WITHOUT touching the right-hand tab
// control, so `*(int*)(*(void**)(book+0xAE0) + 0x34)` keeps whatever the player last selected.
//
//   (a) That is why the % labels showed up over the mandala: the post-Draw pass only tested the
//       RIGHT tab (== 2 Dropping) and happily painted the last mob's numbers onto the cover page.
//       It now reads the LEFT tab first and bails on 9 -- and stops counting the Dropping tab as
//       on screen, so no query is armed from the cover either.
//
//   (b) Per the user's decision the right-hand content tabs are BLOCKED on the cover: from the
//       pump detour, `leftTab == 9 && contentTab != 0` writes the content tab back to 0 and raises
//       the pane-1 dirty flag, so Basic Info is the only page the home screen can ever show.
//       Writing the field directly is exactly what the client itself reads: Draw takes the index
//       from the CCtrlTab at book+0xAE0 (+0x34) at 0x00865792 and there is NO book-side copy of it
//       -- book+0x5B8 is the selected CARD SLOT (GetSelectedCard at 0x00867A2F feeds it to
//       GetSlotCard as the `index` argument) and book+0x5B0 is the card-grid page, neither of them
//       the content tab. The engine's own CCtrlTab setter (0x008603DF) is deliberately NOT called:
//       it is a 700-byte function that re-skins the strip and can call back into the window, which
//       is not something to run from inside the per-frame pump.
//
// ---------------------------------------------------------------------------------------------
// SECTION 6.H / H2 (7) -- BASIC INFO MUST RENDER THE MOB ART AT 0 CARDS. THE SECOND GATE.
// ---------------------------------------------------------------------------------------------
// Round 1 NOP'd the DRAW-side gate (`jle` at 0x00865F9C, "card level <= 0 -> skip the art"), which
// is necessary but not sufficient: with it gone Draw does build the art layer, it just has nothing
// to put in it. The real content gate is one function further back.
//
//   * Draw's Basic Info branch creates a fresh IWzGr2DLayer (0x00865FF5), parents it to the page
//     layer at book+0xB20 (0x00866030 / 0x008660A1), caches it at book+0xE3C -- and then fills it
//     by walking a FRAME LIST whose head is at book+0xE4C:
//
//         0x008661AC  mov  edi, [eax+0xE4C]      ; eax == the book
//         0x008661B2  test edi, edi
//         0x008661B4  je   0x0086635D            ; empty -> insert NOTHING, page stays blank
//
//     each node giving delay (+0x4C), a0 (+0x50), a1 (+0x54) and the canvas (+0x0C) to
//     IWzGr2DLayer::InsertCanvas at 0x008662A9. An EMPTY list is a blank page with working HP/MP --
//     exactly the reported symptom.
//
//   * That list is the ZList object at book+0xE40 (head at +0xC == book+0xE4C). It is wiped on
//     every selection by the clear helper 0x00861AB8 (called at SetMobInfo's head with
//     ecx = book+0xE20; the +0x20 slot it clears via 0x00416B72 IS this list), and refilled by
//     SetMobInfo alone, one of two ways:
//       - from `Mob/<mobId>.img/info/default`'s children if that node exists (loop at 0x00866F01,
//         reading `delay` / `a0` / `a1` per frame). That node is RARE in v83 Mob.wz -- 46 of the
//         2648 mob images in the repo mirror carry it, all of them boss / special mobs -- and an
//         ordinary card mob does not (Snail 0100100 is { info, move, stand, hit1, die1 } with a
//         scalars-only `info`). Mobs that DO have it were never affected by the defect, because
//         this branch is not card-gated at all;
//       - so for essentially every card mob in the book, via the fallback at 0x008671E5:
//
//             lea  eax, [edi+0xE40]              ; &the frame list
//             push eax
//             push dword ptr [edi+0xE20]         ; <-- THE CARD LEVEL
//             push dword ptr [eax+4]             ; mobId
//             call 0x008677CC
//
//   * 0x008677CC (exactly ONE caller, the line above) is a card-level-gated action picker. It
//     appends `(mobId, action)` animations, looked up through the singleton at 0xBE78D4 by
//     0x0040C831, whose action-name table is `[esi*4 + 0xBEC4C8]` (built at 0x004A6C05:
//     0 move, 1 stand, 2 jump, 3 fly, 4 regen, 5 bomb, 6 hit1, 7 hit2, 8 hitF, 9 die1, 10 die2,
//     11 dieF, 12..20 attack1..attackF, 21..37 skill1..skillF, 38 chase, 39 miss):
//
//         level > 0  ->  { 1 stand, 3 fly }          <-- THE SPRITE ITSELF
//         level > 1  ->  { 0 move, 2 jump }
//         level > 2  ->  { 12..20 attack*, 21..37 skill* }
//         level > 3  ->  { 6, 7, 8 hit* }
//         level > 4  ->  { 9, 10 die* }
//
//     So at card level 0 NOT ONE action is appended, the list stays empty, and Basic Info has no
//     art. At level >= 1 `stand` alone is enough to render -- which is precisely the in-game fact
//     the user reported (4 cards draws the sprite, 0 cards draws nothing).
//
//   * FIX: the very first gate, `jle 0x00867811` at 0x008677F7 (bytes 7E 18), is NOP'd, so
//     { stand, fly } is appended unconditionally. Card levels >= 1 are completely unaffected --
//     they already took that path -- and a level-0 mob now renders exactly what a 1-card mob
//     renders today. Nothing else in the picker is touched: the richer sets stay earned. An action
//     the mob has no node for is skipped by the picker's own null test at 0x008678DC, so no new
//     failure mode is introduced. The bytes are verified before the write; on a mismatch the patch
//     is skipped and logged, and the page simply keeps its stock behaviour.
//
//     This is a patch INSIDE a body nobody detours (0x008677CC has no Kentakae hook), so it cannot
//     collide with this module's own Detours trampoline on SetMobInfo.
// ===========================================================================================

#if USE_MONSTER_BOOK_DROPS

namespace {

// ---- verified client addresses --------------------------------------------------------------
constexpr uintptr_t kAddr_CUIMonsterBook_OnDraw = 0x00861E5C;     // __thiscall void Update() -- primary vtable 0x00B3A034 slot 0 (overrides CWnd::Update 0x009E067E)
constexpr uintptr_t kAddr_CUIMonsterBook_Draw = 0x00865782;       // __thiscall void Draw()   -- right-hand page
constexpr uintptr_t kAddr_CUIMonsterBook_SetMobInfo = 0x00866C2C; // __thiscall void (holder, CardRecord*)
constexpr uintptr_t kAddr_DropPagingCmp = 0x008674CF;             // cmp dword ptr [ebp-0x1C], 0x14
// H2(7). First card-level gate of the Basic Info art-action picker (0x008677CC, single caller
// 0x008671FA inside SetMobInfo): `jle 0x00867811` skipping the { 1 stand, 3 fly } append.
constexpr uintptr_t kAddr_BasicInfoArtActionGate = 0x008677F7;
constexpr uintptr_t kAddr_CClientSocket_SendPacket = 0x0049637B;
constexpr uintptr_t kAddr_CClientSocket_Instance = 0x00BE7914;
constexpr uintptr_t kAddr_CUserLocal_Instance = 0x00BEBF98;
constexpr uint32_t kOff_CUserLocal_CharId = 0x11A8; // CUser::GetCharacterId 0x007A6E53 `mov eax,[ecx+0x11A8]` (damageskin.cpp); was 0x19E8, unused in this exe
constexpr uint32_t kOff_CClientSocket_Socket = 0x08;
constexpr uint32_t kOff_CClientSocket_Closing = 0x14;

// `cmp dword ptr [ebp-0x1C], imm8` -- opcode 83 /7 with a disp8 of -0x1C. Verified byte for byte
// before the imm8 is touched, so a client this module was not built for is simply left alone.
constexpr uint8_t kDropPagingSig[3] = { 0x83, 0x7D, 0xE4 };
constexpr uint32_t kOff_DropPagingImm = 3;

// `jle rel8 +0x18`. Two bytes, verified before they are replaced by two NOPs; the `cmp` that sets
// the flags is at 0x008677EE and the `push 3 / mov / pop edi` between them do not touch EFLAGS, so
// removing the branch makes the block unconditional and leaves EDI == 3 for the append below it.
constexpr uint8_t kArtActionGateSig[2] = { 0x7E, 0x18 };

// ---- CUIMonsterBook layout (monsterbook-implementation.md §0/§4b + the reads above) -----------------
constexpr uint32_t kOff_PageIndex = 0x5B4;   // first visible index; for Dropping this is the PAGE
constexpr uint32_t kOff_LeftTabCtrl = 0xAD8; // colour-group tabs; 9 == "no card selected"
constexpr uint32_t kOff_RightTabCtrl = 0xAE0;
constexpr uint32_t kOff_TabSelected = 0x34; // selected index inside either tab control
constexpr uint32_t kOff_PaneDirty = 0xB1C;  // pane[1].dirty of the 3-pane array at +0xB14 (stride 8)
constexpr uint32_t kOff_PageLayer = 0xB20;  // IWzGr2DLayer* the page canvas is composited onto
constexpr uint32_t kOff_SlotRects = 0xCBC;  // RECT[20], the Dropping icon grid
constexpr uint32_t kOff_CardLevel = 0xE20;  // card level of the selected mob (SetMobInfo 0x00866C83)
constexpr uint32_t kOff_ArtFrameHead = 0xE4C; // head of the Basic Info frame ZList at +0xE40 (+0xC)
constexpr int kTabDropping = 2;   // right tab: 0 Basic Info, 1 Episode, 2 Dropping, 3 Found In
constexpr int kTabBasicInfo = 0;  // the only page the cover screen is allowed to show (H2.4b)
constexpr int kLeftTabNoCard = 9; // SetMobInfo's own bail value; also Draw's cover-page divert

// ---- grid geometry ---------------------------------------------------------------------------
// The client builds 20 rects; we keep writing all 20 so no slot can ever hold a stale value, but
// with the paging patch in force only the first 16 are reachable. Slots 16..19 continue the same
// formula onto a would-be 5th row (top 252, bottom 284, still inside the 290 px page canvas), so
// even a hypothetical 17th item lands somewhere sane rather than at (0,0).
constexpr int kRectSlots = 20;          // entries the client's builder writes (0xCBC .. 0xDFB)
constexpr int kGridCols = 4;
constexpr int kSlotsPerPage = 16;       // 4 cols x 4 rows -- must match the patched imm8 below
constexpr uint8_t kStockSlotsPerPage = 20;
constexpr int kCellW = 32;
constexpr int kCellH = 32;
constexpr int kColBase = 38;  // stock: left = 36*(i%4) + 38
constexpr int kColStep = 36;  // stock, unchanged
constexpr int kRowBase = 60;  // stock: top  = 36*(i/4) + 60
constexpr int kRowStep = 48;  // 36 -> 48; 48*3 + 60 + 32 == 236 == the stock bottom edge

// ---- protocol --------------------------------------------------------------------------------
constexpr uint16_t kOp_C2S_MonsterBookQuery = 0x3732; // Server RecvOpcode.MONSTER_BOOK_QUERY (reference used 0x372B)
constexpr uint8_t kQueryType_DropTable = 0;
constexpr uint8_t kResultType_DropTable = 0;
// The dispatcher PEEKS the opcode (Peek2Public), so CurrentPublic() sits ON it and CanRead counts
// from there. Header = [op:2][type:1][mobId:4][n:2]; each entry = [itemId:4][ppm:4].
constexpr size_t kResultHeaderLen = 2 + 1 + 4 + 2;
constexpr size_t kResultEntryLen = 8;
constexpr int kMaxEntries = 2048; // sanity cap; the server caps its own lists far below this

// ---- pacing ----------------------------------------------------------------------------------
constexpr DWORD kQueryRetryMs = 4000; // no reply -> ask again
constexpr int kMaxQueryAttempts = 3;  // ... at most this many times per mob, then give up quietly
constexpr int kMaxRewardEntries = 512;

// ---- label look ------------------------------------------------------------------------------
constexpr unsigned kLabelBg = 0xFFFFFFFF; // opaque white box
constexpr unsigned kLabelFg = 0xFF000000; // black text
constexpr unsigned kFontHeight = 11;      // Dotum 11 == the client's own UI size
constexpr int kLabelPadX = 2;             // px of white on each side of the text
constexpr int kLabelGapY = 1;             // px of air between the icon's bottom edge and the box
constexpr int kLabelMaxW = kColStep;      // never wider than the column pitch -> labels can touch
                                          // at worst, never overlap
constexpr int kLabelMaxH = kRowStep - kCellH - kLabelGapY; // 15 px of the 16 px inter-row gap
constexpr int kLabelMinH = 6;
constexpr int kFallbackPaneW = 220; // page canvas size, if get_width/get_height ever fails
constexpr int kFallbackPaneH = 290;

// ---- stale-slot look (F3.1) -------------------------------------------------------------------
// The plate the stock Dropping pass puts under every icon. It is at the IMAGE ROOT -- there is no
// IconBase under MonsterBook (spec §4b; monsterBookSearch.cpp resolves the identical UOL).
constexpr wchar_t kUOL_IconBase[] = L"UI/UIWindow.img/IconBase/0";
constexpr int kIconBaseMaxDim = 64;         // sanity bound on the art we are willing to blit
constexpr unsigned kStaleWash = 0x80808080; // 50% grey, src-over -- fallback if the plate is gone

// ---- state (main thread only: the packet dispatcher, the flush, the pump and Draw all run there)
struct Chance {
    int itemId;
    int ppm;
};

int g_mobId = 0;               // mob the book is currently showing (SetMobInfo)
int g_chanceMob = 0;           // mob g_chances belongs to; 0 == nothing cached
std::vector<Chance> g_chances; // sorted by itemId
int g_rewardMob = 0;           // mob g_reward belongs to
std::vector<int> g_reward;     // String/MonsterBook.img/<mob>/reward, in draw order
bool g_droppingOnScreen = false;
int g_queryMob = 0;
DWORD g_queryTick = 0;
int g_queryAttempts = 0;

// Raised whenever data the Dropping page renders from becomes available, consumed by the pump
// detour, which turns it into the client's own "this pane is dirty" store. See DEFECT 1 above.
bool g_repaintPending = false;

// Set at attach time only if the 20-per-page imm8 was found AND patched. The rect rewrite is
// gated on it so the grid and the paging can never disagree.
bool g_gridPatched = false;

// H2(7). Set at attach time if the Basic Info art-action gate was byte-verified AND NOP'd.
// Reporting-only: nothing changes behaviour on it, it just makes an `art=0` log line readable.
bool g_artGatePatched = false;

IWzFontPtr g_font;
bool g_fontTried = false;
int g_labelH = 12;

// The empty slot plate, resolved once per session (never from the paint path twice) exactly like
// monsterBookSearch.cpp does. A miss is cached as a miss: no WZ probe loop on a draw path.
IWzCanvasPtr g_iconBase;
bool g_iconBaseTried = false;
int g_iconBaseW = 0;
int g_iconBaseH = 0;

// Prepared labels for the page currently on screen. Draw runs on state changes, not per frame,
// but the string formatting, the CalcTextWidth round trips and the rect maths are still done
// ONCE per (mob, page, reply) so a repaint collapses to at most 16 DrawRectangle + DrawText.
struct SlotLabel {
    bool used;  // a percentage label was prepared for this slot
    bool blank; // F3.1: the server did not report this slot's item -> overpaint the icon
    char text[16];
    int boxX, boxY, boxW, boxH, textX;
    int iconX, iconY, iconW, iconH; // the cell the client blitted the icon into
};

SlotLabel g_labels[kSlotsPerPage] = {};
int g_labelMob = 0;
int g_labelPage = -1;
bool g_labelsReady = false;

void InvalidateLabels() {
    g_labelsReady = false;
    g_labelMob = 0;
    g_labelPage = -1;
}

// ---- originals -------------------------------------------------------------------------------
using VoidThisFn = void(__thiscall*)(void* pThis);
using SetMobInfoFn = void(__thiscall*)(void* pThis, uintptr_t holder, void* pCard);
using SendPacketFn = void(__thiscall*)(void* pSocket, COutPacket* pPacket);

auto CUIMonsterBook_OnDraw = reinterpret_cast<VoidThisFn>(kAddr_CUIMonsterBook_OnDraw);
auto CUIMonsterBook_Draw = reinterpret_cast<VoidThisFn>(kAddr_CUIMonsterBook_Draw);
auto CUIMonsterBook_SetMobInfo = reinterpret_cast<SetMobInfoFn>(kAddr_CUIMonsterBook_SetMobInfo);
auto CClientSocket_SendPacket = reinterpret_cast<SendPacketFn>(kAddr_CClientSocket_SendPacket);

// ---- tiny SEH helpers ------------------------------------------------------------------------
// Kept in their own functions on purpose: MSVC forbids __try in a function that also needs C++
// object unwinding, and every caller below holds vectors / smart pointers.

int SafeReadInt(const void* p, uint32_t off) {
    __try {
        return *reinterpret_cast<const int*>(reinterpret_cast<const char*>(p) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void* SafeReadPtr(const void* p, uint32_t off) {
    __try {
        return *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(p) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Mirrors SetMobInfo's own entry guard (0x00866C56..0x00866C69): NULL record, or left tab 9,
// means the original bails and the page is not re-pointed at this mob.
int ReadSelectedMobId(void* pThis, void* pCard) {
    __try {
        if (!pThis || !pCard) {
            return 0;
        }
        void* pLeftTab = *reinterpret_cast<void**>(reinterpret_cast<char*>(pThis) + kOff_LeftTabCtrl);
        if (!pLeftTab) {
            return 0;
        }
        if (*reinterpret_cast<int*>(reinterpret_cast<char*>(pLeftTab) + kOff_TabSelected) == kLeftTabNoCard) {
            return 0;
        }
        return *reinterpret_cast<int*>(reinterpret_cast<char*>(pCard) + 4); // CardRecord.mobId
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool InGame() {
    __try {
        void* user = *reinterpret_cast<void**>(kAddr_CUserLocal_Instance);
        if (!user) {
            return false;
        }
        return *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(user) + kOff_CUserLocal_CharId) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SocketConnected() {
    __try {
        void* p = *reinterpret_cast<void**>(kAddr_CClientSocket_Instance);
        if (!p) {
            return false;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(p);
        const uint32_t sock = *reinterpret_cast<uint32_t*>(base + kOff_CClientSocket_Socket);
        const uint32_t closing = *reinterpret_cast<uint32_t*>(base + kOff_CClientSocket_Closing);
        return sock != 0 && sock != 0xFFFFFFFF && closing == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- grid geometry ---------------------------------------------------------------------------

constexpr int SlotLeft(int slot) {
    return kColStep * (slot % kGridCols) + kColBase;
}

constexpr int SlotTop(int slot) {
    return kRowStep * (slot / kGridCols) + kRowBase;
}

// What one pump pass actually did, so the hook can log it OUTSIDE the __try (MSVC will not accept
// __try in a frame that needs object unwinding, and keeping the guarded body POD-only is what has
// let this function stay a plain structured-exception frame).
struct FrameActions {
    bool rectsRewritten;  // the 4x4 grid had drifted and was re-asserted
    bool repaintHandled;  // a pending "new data landed" became the client's own dirty flag
    bool contentTabParked;// H2(4b): the cover page had a non-Basic-Info tab selected
    int leftTab;          // raw value seen (-1 == unreadable)
    int contentTabWas;    // raw content-tab value before the park (-1 == unreadable)
};

// One frame of pump work, POD only so it can carry the __try itself:
//   * H2(4b) FIRST: while the cover page (left tab 9) is up, force the right-hand content tab back
//     to Basic Info. It has to happen before the original runs, because the store below feeds the
//     very pane flush this function is about to call through to.
//   * re-assert the 4x4 rect table (idempotent -- one compare, then 80 stores only when it has
//     drifted, e.g. right after OnCreate rebuilt it);
//   * hand a pending "new data landed" over to the client's own pane-dirty flag, which is what
//     makes the labels show up on the FIRST visit (see DEFECT 1 at the top of this file).
// All of it happens BEFORE the original runs, so the pump sees the flags and Draw sees the rects.
void PrepareFrame(void* pThis, FrameActions* out) {
    out->rectsRewritten = false;
    out->repaintHandled = false;
    out->contentTabParked = false;
    out->leftTab = -1;
    out->contentTabWas = -1;
    if (!pThis) {
        return;
    }
    __try {
        char* const book = reinterpret_cast<char*>(pThis);

        // ---- H2(4b): the cover page shows Basic Info and nothing else -------------------------
        // Draw diverts to the cover at 0x0086590E on `leftTab == 9` but never touches the content
        // tab, so the last content selection survives and the cover keeps rendering another mob's
        // page furniture. Both indices live in their CCtrlTab (+0x34), which is exactly where Draw
        // reads them (0x00865792 / 0x0086579E) -- there is no second copy to keep in sync.
        void* const pLeftTab = *reinterpret_cast<void**>(book + kOff_LeftTabCtrl);
        void* const pRightTab = *reinterpret_cast<void**>(book + kOff_RightTabCtrl);
        if (pLeftTab && pRightTab) {
            const int leftTab = *reinterpret_cast<int*>(reinterpret_cast<char*>(pLeftTab) + kOff_TabSelected);
            out->leftTab = leftTab;
            if (leftTab == kLeftTabNoCard) {
                int* const pContentTab =
                        reinterpret_cast<int*>(reinterpret_cast<char*>(pRightTab) + kOff_TabSelected);
                const int contentTab = *pContentTab;
                if (contentTab != kTabBasicInfo) {
                    out->contentTabWas = contentTab;
                    out->contentTabParked = true;
                    *pContentTab = kTabBasicInfo;
                    // The client's own dirty store (SetPage 0x00866AFD, SetSelectedSlot
                    // 0x00866B24). The pump picks it up a few instructions later and repaints the
                    // right pane on this very frame. The write above makes the condition false, so
                    // this fires once per offending click, never per frame.
                    *reinterpret_cast<int*>(book + kOff_PaneDirty) = 1;
                }
            }
        }

        if (g_gridPatched) {
            int* rects = reinterpret_cast<int*>(book + kOff_SlotRects);
            // rect i occupies rects[4i .. 4i+3] = {left, top, right, bottom}. Row 0 is identical
            // to stock, so probe a row the new step actually moves: slot 4 (row 1) is at
            // top 108 for us and 96 for the client's builder, slot 12 (row 3) at 204 vs 168.
            if (rects[4 * 4 + 1] != SlotTop(4) || rects[12 * 4 + 1] != SlotTop(12)) {
                out->rectsRewritten = true;
                for (int slot = 0; slot < kRectSlots; ++slot) {
                    int* rc = rects + slot * 4;
                    const int left = SlotLeft(slot);
                    const int top = SlotTop(slot);
                    rc[0] = left;
                    rc[1] = top;
                    rc[2] = left + kCellW;
                    rc[3] = top + kCellH;
                }
            }
        }
        if (g_repaintPending) {
            g_repaintPending = false;
            out->repaintHandled = true;
            // The identical store the client performs in SetPage (0x00866AFD) and SetSelectedSlot
            // (0x00866B24). The pump two instructions later picks it up and repaints the pane.
            *reinterpret_cast<int*>(book + kOff_PaneDirty) = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---- cache -----------------------------------------------------------------------------------

void ResetForNewMob(int mobId) {
    g_mobId = mobId;
    g_chanceMob = 0;
    g_chances.clear();
    g_rewardMob = 0;
    g_reward.clear();
    g_queryMob = 0;
    g_queryTick = 0;
    g_queryAttempts = 0;
    InvalidateLabels();
}

bool LookupChance(int itemId, int* outPpm) {
    const auto it = std::lower_bound(g_chances.begin(), g_chances.end(), itemId,
            [](const Chance& c, int id) { return c.itemId < id; });
    if (it == g_chances.end() || it->itemId != itemId) {
        return false;
    }
    *outPpm = it->ppm;
    return true;
}

// Reads the same list the client paged into the Dropping tab. Non-throwing WZ probes
// (wvs/util.h): a missing node is an ordinary answer here, not an error.
void FillRewardCache(int mobId) {
    g_reward.clear();
    g_rewardMob = mobId; // even a miss is cached, so we probe the WZ once per selection
    if (mobId <= 0) {
        return;
    }
    try {
        wchar_t path[96];
        _snwprintf_s(path, _countof(path), _TRUNCATE, L"String/MonsterBook.img/%d/reward", mobId);
        Ztl_variant_t vReward = get_object_or_empty(path);
        IWzPropertyPtr pReward = vReward.GetUnknown();
        if (!pReward) {
            return;
        }
        wchar_t key[16];
        for (int i = 0; i < kMaxRewardEntries; ++i) {
            _snwprintf_s(key, _countof(key), _TRUNCATE, L"%d", i);
            Ztl_variant_t v = get_item_or_empty(pReward, key);
            if (v.vt == VT_EMPTY || v.vt == VT_ERROR) {
                break; // the list is contiguous: the first hole is the end
            }
            const int itemId = get_int32(v, 0);
            if (itemId <= 0) {
                break;
            }
            g_reward.push_back(itemId);
        }
    } catch (...) {
        // a partially filled list is still correct for the slots it covers
    }
}

// ---- drawing ---------------------------------------------------------------------------------

IWzFont* EnsureFont() {
    if (g_font) {
        return g_font;
    }
    if (g_fontTried) {
        return nullptr; // one attempt per session; a retry loop on the draw path is worse than no label
    }
    g_fontTried = true;
    try {
        PcCreateObject<IWzFontPtr>(L"Canvas#Font", g_font, nullptr);
        if (!g_font) {
            return nullptr;
        }
        Ztl_bstr_t sName(L"Dotum");
        if (FAILED(g_font->raw_Create(sName, kFontHeight, kLabelFg, vtEmpty))) {
            g_font = nullptr;
            return nullptr;
        }
        int fullHeight = 0;
        if (SUCCEEDED(g_font->get_fullHeight(&fullHeight)) && fullHeight >= 9 && fullHeight <= 20) {
            g_labelH = fullHeight;
        }
    } catch (...) {
        g_font = nullptr;
    }
    if (!g_font) {
        DEBUG_MESSAGE("[monsterBookDrops] font creation failed -- labels disabled for this session\n");
    }
    return g_font;
}

// F3.1. The empty slot plate. Resolved at most ONCE per session -- a failure is remembered, so a
// client whose UIWindow.img has no IconBase never re-probes the WZ from the draw path; the caller
// falls back to the grey wash. Non-throwing probe (wvs/util.h): a missing node is a plain answer.
IWzCanvas* EnsureIconBase() {
    if (g_iconBase) {
        return g_iconBase;
    }
    if (g_iconBaseTried) {
        return nullptr;
    }
    g_iconBaseTried = true;
    try {
        Ztl_variant_t v = get_object_or_empty(kUOL_IconBase);
        if (v.vt == VT_UNKNOWN || v.vt == VT_DISPATCH) {
            g_iconBase = IWzCanvasPtr(v.GetUnknown(false, false));
        }
        if (g_iconBase) {
            unsigned w = 0, h = 0;
            if (SUCCEEDED(g_iconBase->get_width(&w)) && SUCCEEDED(g_iconBase->get_height(&h))
                    && w > 0 && h > 0 && static_cast<int>(w) <= kIconBaseMaxDim
                    && static_cast<int>(h) <= kIconBaseMaxDim) {
                g_iconBaseW = static_cast<int>(w);
                g_iconBaseH = static_cast<int>(h);
            } else {
                g_iconBase = nullptr; // unmeasurable -> unusable; the wash is the safer answer
            }
        }
    } catch (...) {
        g_iconBase = nullptr;
    }
    if (!g_iconBase) {
        DEBUG_MESSAGE("[monsterBookDrops] IconBase unavailable -- stale slots fall back to a grey wash\n");
    }
    return g_iconBase;
}

// Widest first. The spec asks for two decimals ("0.35%"), which is what almost every drop gets;
// the narrower forms only exist so a wide number cannot outgrow the 36 px column pitch and run
// into its neighbour. "<0.01%" is kept as the FIRST choice for sub-0.005% drops (round-1
// feedback: printing "0.00%" for a drop that does exist is a lie); "<.01%" is only its fallback.
int BuildCandidates(int ppm, char cand[3][16]) {
    const double pct = static_cast<double>(ppm) / 10000.0;
    int n = 0;
    if (pct >= 100.0) {
        sprintf_s(cand[n++], 16, "100%%");
        return n;
    }
    if (ppm > 0 && pct < 0.005) {
        sprintf_s(cand[n++], 16, "<0.01%%");
        sprintf_s(cand[n++], 16, "<.01%%");
        return n;
    }
    sprintf_s(cand[n++], 16, "%.2f%%", pct);
    if (pct >= 10.0) {
        sprintf_s(cand[n++], 16, "%.1f%%", pct);
        sprintf_s(cand[n++], 16, "%.0f%%", pct);
    }
    return n;
}

unsigned MeasureText(IWzFont* pFont, const char* text) {
    unsigned width = 0;
    Ztl_bstr_t s(text);
    if (FAILED(pFont->raw_CalcTextWidth(s, vtEmpty, &width)) || width == 0 || width > 4096) {
        width = static_cast<unsigned>(strlen(text)) * 6u; // Dotum 11 digits are ~6px
    }
    return width;
}

// One-off prep for the page currently on screen: pick the text, measure it, and place the box in
// the gap UNDER the icon, on the SAME rect the stock Draw blitted the icon from (this + 0xCBC +
// slot*0x10, read at runtime -- so even though we wrote that table ourselves, the label is
// positioned from what the client actually used and can never drift from the icon).
void RebuildLabels(void* pThis, IWzFont* pFont, int page, int paneW, int paneH) {
    for (int slot = 0; slot < kSlotsPerPage; ++slot) {
        g_labels[slot].used = false;
        g_labels[slot].blank = false;
    }

    for (int slot = 0; slot < kSlotsPerPage; ++slot) {
        const size_t idx = static_cast<size_t>(page) * kSlotsPerPage + static_cast<size_t>(slot);
        if (idx >= g_reward.size()) {
            break; // past the end of the mob's drop list -> the client drew no icon here either
        }

        // The rect the stock Draw actually blitted this slot's icon from. Read FIRST, because both
        // outcomes below need it: a label is placed under it, a stale icon is overpainted on it.
        const uint32_t rcOff = kOff_SlotRects + static_cast<uint32_t>(slot) * 16u;
        const int left = SafeReadInt(pThis, rcOff + 0);
        const int top = SafeReadInt(pThis, rcOff + 4);
        const int right = SafeReadInt(pThis, rcOff + 8);
        const int bottom = SafeReadInt(pThis, rcOff + 12);
        if (right <= left || bottom <= top) {
            continue;
        }
        if (left < 0 || top < 0 || right > paneW || bottom > paneH) {
            continue; // never paint outside the page canvas
        }

        int ppm = 0;
        if (!LookupChance(g_reward[idx], &ppm) || ppm <= 0) {
            // F3.1. The reply for THIS mob is already in (PaintLabels will not call us otherwise),
            // and it does not mention this item: the drop is gone from drop_data, or the server
            // declines to report it. The baked icon is therefore stale -- blank the cell instead of
            // leaving an unlabelled icon that reads as "still loading". Never a guess: with no
            // reply we are not reached at all.
            SlotLabel& lab = g_labels[slot];
            lab.blank = true;
            lab.iconX = left;
            lab.iconY = top;
            lab.iconW = right - left;
            lab.iconH = bottom - top;
            continue;
        }

        char cand[3][16];
        const int nCand = BuildCandidates(ppm, cand);
        const int maxTextW = kLabelMaxW - 2 * kLabelPadX;
        int chosen = nCand - 1;
        unsigned textW = 0;
        for (int c = 0; c < nCand; ++c) {
            const unsigned w = MeasureText(pFont, cand[c]);
            if (static_cast<int>(w) <= maxTextW || c == nCand - 1) {
                chosen = c;
                textW = w;
                break;
            }
        }

        int boxW = static_cast<int>(textW) + 2 * kLabelPadX;
        if (boxW < 8) {
            boxW = 8;
        }
        if (boxW > kLabelMaxW) {
            boxW = kLabelMaxW;
        }
        int boxH = g_labelH;
        if (boxH > kLabelMaxH) {
            boxH = kLabelMaxH;
        }
        if (boxH < kLabelMinH) {
            continue;
        }

        // BELOW the icon and OUTSIDE it (spec §6.B): the box starts one pixel under the cell's
        // bottom edge and lives entirely in the 16 px of air the dropped row paid for.
        int boxX = left + ((right - left) - boxW) / 2; // centred on the icon
        int boxY = bottom + kLabelGapY;
        if (boxX + boxW > paneW) {
            boxX = paneW - boxW;
        }
        if (boxX < 0) {
            boxX = 0;
        }
        if (boxY < 0 || boxY + boxH > paneH) {
            continue; // no room under this cell -> no label, never a clipped or shifted one
        }

        int textX = boxX + (boxW - static_cast<int>(textW)) / 2;
        if (textX < boxX) {
            textX = boxX;
        }

        SlotLabel& lab = g_labels[slot];
        lab.used = true;
        lab.boxX = boxX;
        lab.boxY = boxY;
        lab.boxW = boxW;
        lab.boxH = boxH;
        lab.textX = textX;
        strcpy_s(lab.text, sizeof(lab.text), cand[chosen]);
    }
}

// Post-Draw pass. `pThis` is the CUIMonsterBook whose right-hand page the stock Draw just
// composited onto (*(this+0xB20))->canvas[0]; we paint on top of that same canvas, in the same
// coordinates, and the next Draw wipes it and we repaint. Nothing accumulates.
void PaintLabels(void* pThis) {
    // MANDATORY (spec §6 shared rule): the item-search result view owns this page and its icons
    // must carry no percentages. Nothing below this line may run while it is up -- and we do not
    // arm a drop query for it either, so the tab is treated as not on screen.
    if (MonsterBookSearch_IsItemResultView()) {
        g_droppingOnScreen = false;
        return;
    }
    // Without the paging patch the client still pages 20 per page while every index below assumes
    // 16, so slot -> reward -> chance would silently pair an icon with another item's percentage.
    // A wrong number is worse than no number: the whole label pass (and the query that feeds it)
    // stays off on a client this module's signature did not match.
    if (!g_gridPatched) {
        g_droppingOnScreen = false;
        return;
    }
    if (!pThis) {
        return;
    }
    // H2(4a). THE COVER PAGE. Left tab 9 is the "Book Master Lvl" mandala, and Draw diverts to it
    // at 0x0086590E without clearing the content tab -- so a right tab still reading 2 (Dropping)
    // is a stale leftover, not a Dropping page. Painting percentages from it is what put the %
    // labels over the mandala in the user's screenshot. The pump detour additionally forces the
    // content tab back to Basic Info here (see PrepareFrame), but this bail is independent of that
    // and holds even on the frame before the park lands.
    void* pLeftTabCtrl = SafeReadPtr(pThis, kOff_LeftTabCtrl);
    if (!pLeftTabCtrl) {
        return; // cannot tell which screen this is -> paint nothing
    }
    const int leftTab = SafeReadInt(pLeftTabCtrl, kOff_TabSelected);
    if (leftTab == kLeftTabNoCard) {
        g_droppingOnScreen = false; // and arm no query from the cover either
        return;
    }
    void* pTabCtrl = SafeReadPtr(pThis, kOff_RightTabCtrl);
    if (!pTabCtrl) {
        return;
    }
    if (SafeReadInt(pTabCtrl, kOff_TabSelected) != kTabDropping) {
        g_droppingOnScreen = false;
        return;
    }
    g_droppingOnScreen = true; // the flush turns this into (at most) one query per mob

    // THE gate for everything below, labels and blanks alike: a reply for the mob on screen has
    // landed AND we know the baked icon list it has to be matched against. Until then nothing is
    // drawn and nothing is erased -- an un-answered query must never be mistaken for "no drops".
    if (g_mobId <= 0 || g_chanceMob != g_mobId || g_rewardMob != g_mobId) {
        return; // reply and/or reward list not in yet -> draw nothing at all (no placeholder)
    }
    // g_chances MAY legitimately be empty here: that is the server answering "this mob drops
    // nothing", which is precisely the case F3.1 has to render (every baked icon is stale). Only
    // an empty BAKED list is a reason to stop -- there are then no icons on the page to reconcile.
    if (g_reward.empty()) {
        return;
    }
    const int page = SafeReadInt(pThis, kOff_PageIndex);
    if (page < 0 || page > 4096) {
        return;
    }

    IWzGr2DLayer* pLayer = reinterpret_cast<IWzGr2DLayer*>(SafeReadPtr(pThis, kOff_PageLayer));
    if (!pLayer) {
        return;
    }
    IWzFont* pFont = EnsureFont();
    if (!pFont) {
        return;
    }

    IWzCanvasPtr pCanvas;
    {
        IWzCanvas* raw = nullptr;
        if (FAILED(pLayer->get_canvas(vtEmpty, &raw)) || !raw) {
            return;
        }
        pCanvas.Attach(raw); // get_canvas hands back an AddRef'd pointer
    }

    if (!g_labelsReady || g_labelMob != g_mobId || g_labelPage != page) {
        int paneW = kFallbackPaneW;
        int paneH = kFallbackPaneH;
        unsigned w = 0, h = 0;
        if (SUCCEEDED(pCanvas->get_width(&w)) && w > 0 && w < 4096) {
            paneW = static_cast<int>(w);
        }
        if (SUCCEEDED(pCanvas->get_height(&h)) && h > 0 && h < 4096) {
            paneH = static_cast<int>(h);
        }
        RebuildLabels(pThis, pFont, page, paneW, paneH);
        g_labelMob = g_mobId;
        g_labelPage = page;
        g_labelsReady = true;
    }

    // F3.1 first: the stale cells are erased before any label goes down, so a blank can never be
    // painted over a neighbour's box. IconBase is resolved lazily and at most once per session.
    IWzCanvas* pPlate = nullptr;
    for (int slot = 0; slot < kSlotsPerPage; ++slot) {
        if (g_labels[slot].blank) {
            pPlate = EnsureIconBase();
            break;
        }
    }
    for (int slot = 0; slot < kSlotsPerPage; ++slot) {
        const SlotLabel& lab = g_labels[slot];
        if (!lab.blank) {
            continue;
        }
        if (pPlate && g_iconBaseW <= lab.iconW && g_iconBaseH <= lab.iconH) {
            // Exactly the blit the stock Dropping pass makes at 0x00865CF7/0x00865D1B before it
            // stamps the item icon on top -- so the cell ends up as a plain empty slot. The plate
            // measures 32x32 at origin (0,0) in this client's Data (read off disk 2026-08-17),
            // i.e. exactly one cell, so it covers the icon and nothing else. The size test is what
            // keeps a client that ships a LARGER plate on the grey wash instead of overpainting a
            // neighbour.
            pCanvas->raw_Copy(lab.iconX, lab.iconY, pPlate, vtEmpty);
        } else {
            // No plate (or art bigger than the cell): dim the icon instead of erasing it.
            // DrawRectangle blends src-over on a partial alpha, so this greys rather than fills.
            pCanvas->raw_DrawRectangle(lab.iconX, lab.iconY,
                    static_cast<unsigned>(lab.iconW), static_cast<unsigned>(lab.iconH), kStaleWash);
        }
    }

    for (int slot = 0; slot < kSlotsPerPage; ++slot) {
        const SlotLabel& lab = g_labels[slot];
        if (!lab.used) {
            continue;
        }
        pCanvas->raw_DrawRectangle(lab.boxX, lab.boxY,
                static_cast<unsigned>(lab.boxW), static_cast<unsigned>(lab.boxH), kLabelBg);
        unsigned drawn = 0;
        Ztl_bstr_t sText(lab.text);
        pCanvas->raw_DrawText(lab.textX, lab.boxY, sText, pFont, vtEmpty, vtEmpty, &drawn);
    }
}

void PaintLabelsInner(void* pThis) {
    try {
        PaintLabels(pThis);
    } catch (...) {
        // a COM/WZ throw here must never reach the engine's draw loop
    }
}

void PaintLabelsGuarded(void* pThis) {
    __try {
        PaintLabelsInner(pThis);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---- hooks -----------------------------------------------------------------------------------

// vtable slot 21 -- the per-frame Draw override. It flushes the three dirty panes and then calls
// CUIWnd::Draw. We only prepare state here (rect table + pane invalidation) and never draw.
void __fastcall CUIMonsterBook_OnDraw_hook(void* pThis, void* /*edx*/) {
    MUSH_FEATURE("monsterBookDrops:onDraw");
    FrameActions acts;
    PrepareFrame(pThis, &acts);
    // Logged out here, never inside the guarded body. Only transitions log, so the per-frame pump
    // stays silent when nothing happened.
    CUIMonsterBook_OnDraw(pThis);
}

void __fastcall CUIMonsterBook_Draw_hook(void* pThis, void* /*edx*/) {
    MUSH_FEATURE("monsterBookDrops:draw");
    CUIMonsterBook_Draw(pThis);
    PaintLabelsGuarded(pThis);
}

void __fastcall CUIMonsterBook_SetMobInfo_hook(void* pThis, void* /*edx*/, uintptr_t holder, void* pCard) {
    MUSH_FEATURE("monsterBookDrops:setMobInfo");
    // Read BEFORE calling through: the 8-byte holder is destroyed inside the original (0x00685F54
    // releases the record), so pCard may not survive the call.
    const int mobId = ReadSelectedMobId(pThis, pCard);
    CUIMonsterBook_SetMobInfo(pThis, holder, pCard);
    if (mobId > 0 && mobId != g_mobId) {
        ResetForNewMob(mobId);
    }
    // H2(7). The outcome of the art build, read from the two members the client itself uses:
    // +0xE20 is the card level SetMobInfo just stored (0x00866C83), +0xE4C is the head of the frame
    // list Draw's Basic Info branch tests at 0x008661AC. `art=0` with the gate patched in means the
    // mob genuinely has no `stand`/`fly` node, NOT that the card level suppressed it.
    const int cardLevel = SafeReadInt(pThis, kOff_CardLevel);
    const void* pFrames = SafeReadPtr(pThis, kOff_ArtFrameHead);
}

// 20 -> 16 items per Dropping page. Verified byte for byte first: a client whose builder does not
// look like `cmp dword ptr [ebp-0x1C], 0x14` at this address is left completely alone, and the
// rect rewrite stays off with it so the two can never disagree.
bool PatchDropPaging() {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(kAddr_DropPagingCmp);
    for (size_t i = 0; i < sizeof(kDropPagingSig); ++i) {
        if (p[i] != kDropPagingSig[i]) {
            return false;
        }
    }
    if (p[kOff_DropPagingImm] != kStockSlotsPerPage) {
        return false;
    }
    Patch1(kAddr_DropPagingCmp + kOff_DropPagingImm, static_cast<unsigned char>(kSlotsPerPage));
    return true;
}

// H2(7). Remove the `card level > 0` gate around the { stand, fly } append in the Basic Info art
// action picker (0x008677CC), so the mob sprite is built at 0 collected cards. Two bytes, verified
// first; on a mismatch nothing is written and the page keeps its stock behaviour. See the long
// section at the top of this file for the full derivation.
bool PatchBasicInfoArtActionGate() {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(kAddr_BasicInfoArtActionGate);
    for (size_t i = 0; i < sizeof(kArtActionGateSig); ++i) {
        if (p[i] != kArtActionGateSig[i]) {
            return false;
        }
    }
    PatchNop(kAddr_BasicInfoArtActionGate,
            kAddr_BasicInfoArtActionGate + sizeof(kArtActionGateSig)); // jle rel8 -> 2x nop
    return true;
}

} // namespace

#endif // USE_MONSTER_BOOK_DROPS

// Main-thread flush (the mod's main-thread flush). Everything that touches the
// socket or the WZ happens HERE, never on a draw or packet path.
void MonsterBookDrops_OnClientTick() {
#if USE_MONSTER_BOOK_DROPS
    const int mob = g_mobId;
    if (mob <= 0 || !g_droppingOnScreen) {
        return; // nothing selected, or the Dropping tab is not the one on screen
    }
    if (g_rewardMob != mob) {
        FillRewardCache(mob); // one WZ probe per selection; keeps Draw free of WZ work
        InvalidateLabels();
        g_repaintPending = true; // half the inputs changed -> let the page repaint when it can
    }
    if (g_chanceMob == mob) {
        return; // the table is already cached
    }
    if (g_queryAttempts >= kMaxQueryAttempts) {
        return; // a server that never answers must not be polled forever
    }
    const DWORD now = GetTickCount();
    if (g_queryMob == mob && g_queryTick != 0 && (now - g_queryTick) < kQueryRetryMs) {
        return; // still in flight
    }
    if (!InGame() || !SocketConnected()) {
        return;
    }
    void* pSocket = *reinterpret_cast<void**>(kAddr_CClientSocket_Instance);
    if (!pSocket) {
        return;
    }
    COutPacket packet(kOp_C2S_MonsterBookQuery);
    packet.Encode1(kQueryType_DropTable);
    packet.Encode4(static_cast<unsigned int>(mob));
    CClientSocket_SendPacket(pSocket, &packet);
    g_queryMob = mob;
    g_queryTick = now;
    ++g_queryAttempts;
    DEBUG_MESSAGE("[monsterBookDrops] query mob=%d attempt=%d\n", mob, g_queryAttempts);
#endif
}

// S2C 0x372C MONSTER_BOOK_RESULT, routed from features/net/packetDispatcher.cpp.
// NON-CONSUMING on purpose: monsterBookSearch.cpp reads the same CInPacket right after us, so we
// walk a raw pointer under CanRead guards and never move the packet's own offset.
void MonsterBookDrops_OnPacket(CInPacket* pPacket) {
#if USE_MONSTER_BOOK_DROPS
    if (!pPacket || !pPacket->CanRead(kResultHeaderLen)) {
        return;
    }
    const uint8_t* p = pPacket->Current();
    if (!p) {
        return;
    }
    if (p[2] != kResultType_DropTable) {
        return; // types 1 / 2 belong to the search module
    }

    int mobId = 0;
    unsigned short count = 0;
    memcpy(&mobId, p + 3, sizeof(mobId));
    memcpy(&count, p + 7, sizeof(count));
    if (mobId <= 0 || count > kMaxEntries) {
        return;
    }
    if (mobId != g_mobId) {
        return; // a late reply for a mob the player already clicked away from -- never let it
                // evict the table the page is currently drawing from
    }
    if (!pPacket->CanRead(kResultHeaderLen + static_cast<size_t>(count) * kResultEntryLen)) {
        return;
    }

    std::vector<Chance> rows;
    rows.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t* e = p + kResultHeaderLen + static_cast<size_t>(i) * kResultEntryLen;
        int itemId = 0;
        int ppm = 0;
        memcpy(&itemId, e + 0, sizeof(itemId));
        memcpy(&ppm, e + 4, sizeof(ppm));
        if (itemId > 0 && ppm > 0) {
            rows.push_back(Chance{ itemId, ppm });
        }
    }
    // itemId is the lookup key on the draw path, and the server sorts by chance, not by id
    std::sort(rows.begin(), rows.end(), [](const Chance& a, const Chance& b) { return a.itemId < b.itemId; });
    rows.erase(std::unique(rows.begin(), rows.end(),
                       [](const Chance& a, const Chance& b) { return a.itemId == b.itemId; }),
            rows.end());

    g_chances.swap(rows);
    g_chanceMob = mobId; // an EMPTY table is still an answer: cache it so we stop asking
    InvalidateLabels();
    // THE fix for "labels only on the second visit": the page was painted and its dirty flag was
    // cleared one round trip ago, so without this the client would never repaint it and the
    // numbers would sit in the cache unseen. The pump detour turns this into the client's own
    // this+0xB1C = 1 on the next frame. NO packet is sent and NO drawing happens from here.
    g_repaintPending = true;
    if (g_queryMob == mobId) {
        g_queryMob = 0;
        g_queryAttempts = 0;
    }
    DEBUG_MESSAGE("[monsterBookDrops] result mob=%d entries=%u\n", mobId, (unsigned)g_chances.size());
#else
    (void)pPacket;
#endif
}

void AttachMonsterBookDropsMod() {
#if USE_MONSTER_BOOK_DROPS
    // 20 -> 16 rows per Dropping page. Must come first: the rect rewrite is gated on it, so if
    // the signature ever stops matching the tab quietly keeps its stock 4x5 grid.
    g_gridPatched = PatchDropPaging();
    if (!g_gridPatched) {
        DEBUG_MESSAGE("[monsterBookDrops] paging imm8 signature mismatch at 0x%08X -- keeping the stock 4x5 grid\n",
                (unsigned)kAddr_DropPagingCmp);
    }

    // H2(7). The SECOND art gate: the card-level test around the { stand, fly } append inside the
    // Basic Info art-action picker. Round 1 only removed the Draw-side one at 0x00865F9C
    // (monsterBook.cpp owns that NOP -- it is not touched here).
    g_artGatePatched = PatchBasicInfoArtActionGate();

    // Per-frame pump (vtable slot 21). Re-asserts the 4x4 rect table and converts "new data
    // landed" into the client's own pane-dirty flag. Never draws, never sends.
    ATTACH_HOOK(CUIMonsterBook_OnDraw, CUIMonsterBook_OnDraw_hook);
    // Post-Draw label pass. The original runs FIRST -- it rebuilds and re-composites the whole
    // 220x290 page, so anything we paint before it would be wiped.
    ATTACH_HOOK(CUIMonsterBook_Draw, CUIMonsterBook_Draw_hook);
    // Selection tracking: the mob id for the query + cache invalidation. Nothing engine-side.
    ATTACH_HOOK(CUIMonsterBook_SetMobInfo, CUIMonsterBook_SetMobInfo_hook);
#endif
}
