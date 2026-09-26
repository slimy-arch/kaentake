#include "pch.h"
#include "constants.h"
#include "debug.h"
#include "hook.h"
#include "worldmapdata.h"                 // GetMobNameById -- String/Mob.img name resolver
#include "wvs/iteminfo.h"                  // CItemInfo::DrawItemIconForSlot
#include "wvs/packet.h"                    // CInPacket / COutPacket
#include "wvs/util.h"                      // get_object_or_empty / get_item_or_empty / get_int32
#include "ztl/ztl.h"                       // IWz*Ptr / PcCreateObject / Ztl_bstr_t / vtEmpty

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>

// =================================================================================================
// MONSTER BOOK -- the two search rows, INSIDE the book.
//
// ROUND 9 == THE CRASH, THE DRAG BAND, THE GHOST STRIP AND THE FALSE RESET (spec 6.J A/B/C/D).
// Round 8's pixel-ownership rules (6.I) still stand; 6.J only overrides them where it says so.
//
//   J-A  `CItemInfo::GetItemName` was pointed at 0x005D5D95, which is the item-SLOT factory and
//        writes through an EIGHT-byte `out`. A four-byte `ZXString<char>` was handed to it, so it
//        stored four bytes past the variable -- "Run-Time Check Failure #2, stack around 'out' was
//        corrupted", on every path that resolves an item name. The name getter is 0x005CF63E, and
//        the whole file's raw engine calls have been re-proved against their callees (the ABI sweep
//        table below the address block). An RTC epilogue check is invisible to `catch(...)`, so
//        correctness was the only available fix.
//   J-B  the top strip (window y 9..25) NEVER receives WM_LBUTTONDOWN -- the window-drag capture
//        takes 0x201 first. Everything up there now fires on the 0x202 UP; row 1 stays on DOWN.
//   J-C  the strip was painted onto a canvas the window does not show. `CWnd::GetCanvas`
//        (0x00425C4C) is now asked for the surface instead of it being re-derived, and the box is
//        repainted on every home<->tier transition in both directions.
//   J-D  the 750 ms "no Update -> the book is gone" reset is DELETED: a modal dialog froze the pump
//        and it wiped a live book's state seventeen seconds before the real DESTROY.
//
// Binding spec: monsterbook-implementation.md sections 6.I and 6.J. Where 6.I contradicts anything older
// (notably H1.2's hosting-row text draw and H1.10's shared magnifier), 6.I wins; where 6.J
// contradicts 6.I, 6.J wins.
//
// -------------------------------------------------------------------------------------------------
// ROUND 8: ONE PAINTER PER PIXEL. Everything else in this round follows from that.
// -------------------------------------------------------------------------------------------------
// The in-game evidence was three code paths writing the same pixels:
//   * the field rows were painted BY THE MODULE *and* by the live CCtrlEdit, so "gloves for att"
//     appeared doubled and the caret sat one character ahead of the glyphs (image 4 proves the
//     control's own text and caret DO composite above our layer -- so round 6's "invisible typed
//     text" diagnosis is retracted, and H1.2's hosting-row text draw is DELETED here);
//   * the home screen's level control and the tier screens' item row shared one window-canvas slot
//     (48,9) AND one physical control (BtSearch2 at 175,9), so the level dropdown rendered on tier
//     screens over the item field (images 1 and 3).
//
// The rule, implemented literally:
//
//   row HOSTING the edit      -> module blits *Box1 ONLY. No text, ever. The edit owns the glyphs
//                                and the caret.
//   row NOT hosting, empty    -> module blits *Box0 (the baked placeholder). No text.
//   row NOT hosting, has text -> module blits *Box1 and draws THE STASH. The edit is not here.
//
//   left tab 0..8 -> the item row and its magnifier exist; the level control does not, and
//                    BtSearch2 is PARKED OFF-SCREEN whenever the book is not on a tier screen so
//                    it cannot swallow the level magnifier's click (CWnd::HitTest does NOT consult
//                    the shown flag -- 0x009E01E7 walks children and calls IsHit and nothing else).
//   left tab 9    -> the level control exists and draws its OWN magnifier; the item row does not.
//   unknown       -> NEITHER paints. `LeftTabOrUnknown` never guesses.
//
// A per-tick assertion re-checks that split from scratch, so a single bad frame cannot leave one
// screen's control alive on the other's.
//
// -------------------------------------------------------------------------------------------------
// WHY THIS IS A REWRITE AND NOT ANOTHER PATCH LAYER
// -------------------------------------------------------------------------------------------------
// Round 5 shipped: a second runtime CCtrlEdit for the top row, a double-click gate on every icon,
// a `g_disabled` session latch, four `CachedSprite::tried` once-only WZ probes, a once-per-book
// control-creation latch, and DEBUG_MESSAGE-only diagnostics that nobody outside a debugger sees.
// In game the result was: typing leaked to the global hotkeys, both boxes rendered white and empty,
// clicking an item result stopped opening the droppers, there was no pagination, the tab strip
// never hid, hovering popped the underlying mob's tooltip -- and after a handful of open/close
// cycles the whole feature went dead until the game was restarted.
//
// That last symptom is the tell. EVERY entry point in round 5 opened with `if (g_disabled) ...`,
// and `g_disabled` was set by SIX different guards (three paint wrappers, the tooltip, the tick's
// catch, the quarantine probe) and cleared by NOTHING. One tripped guard therefore took out the
// second magnifier (it is created on the tick, which returns immediately when disabled), the mob
// search (OnButtonClicked passes 0x7D0 straight to the stock exact-name jump when disabled), the
// hover, the tab strip and the item view -- all at once, permanently, exactly as reported. So:
//
//   * `g_disabled` IS DELETED. Nothing in this file can disable the feature for the session.
//   * every other latch is keyed on a per-book GENERATION counter that OnCreate bumps, so a failed
//     probe / creation / patch retries on the next open (spec 6.G G0.2).
//   * every guard, every probe, every engine call and every state transition writes a line to
//     a temporary session trace so the next repro is measured, not guessed.
//
// -------------------------------------------------------------------------------------------------
// THE CLIENT FACTS THIS FILE STANDS ON  (all re-derived from MapleStory.exe @0x00400000 for round 6)
// -------------------------------------------------------------------------------------------------
// CUIMonsterBook::RefreshCtrls (0x00863C49), read instruction by instruction:
//     leftTab   = [book+0xAD8]->[+0x34]        bHide = (leftTab == 9)      show = !bHide
//     contTab   = [book+0xAE0]->[+0x34]
//     SetShow(show)  on  0xAF0 BtSearch, 0xAF8 (0x7D1), 0xB00 (0x7D2), 0xB08 (0x7D3),
//                        0xB10 (0x7D4)  and  0xAE8 the edit             (slot 9 on ctrl+4, +0x24)
//     SetShow AGAIN  on  0xB08 and 0xB10 with (leftTab != 9 && contTab != 0)
//                                                          (0x00863CC7 .. 0x00863CFE)
//     SetEnable      on  0xAF0 = show, 0xAF8 = show && cardPage != 0,
//                        0xB00 = show && cardPage < nPages-1,
//                        0xB08 = show && listPage != 0,
//                        0xB10 = show && listPage < listLen-1           (slot 7 on ctrl+4, +0x1C)
//
// CORRECTION (round-6 audit, re-read at 0x00863CC7): round 6's own header used to claim the
// RIGHT-page pagers are "already visible on every colour tab" and that RefreshCtrls "only
// DISABLES them". That is WRONG -- there is a SECOND SetShow pass on 0xB08/0xB10 keyed on
// `contTab != 0`, so on Basic Info the client genuinely HIDES them. Round 5's note was right.
// The CODE was never wrong (EnforceListArrows asserts BOTH Show and Enable, which is what the
// item view needs); only this comment was, and the next round must not inherit it.
//
// The stock controls, from the factory (0x008625C4..0x008629CC), each `CreateCtrl` read off its
// own push sequence:
//     0xAD8 CCtrlTab   nId 0x7D6  rect {-7, 25, 50, 305}     (the colour tabs, left edge)
//     0xAE0 CCtrlTab   nId 0x7D7  rect {439, 25, 506, 220}   (the four content tabs, right edge)
//     0x070 CCtrlButton nId 0x3E8 (429,  8)  BtClose
//     0xAF0 CCtrlButton nId 0x7D0 (175, 29)  BtSearch        <- the MOB row's magnifier
//     0xAF8 CCtrlButton nId 0x7D1 ( 48,285)  card page -
//     0xB00 CCtrlButton nId 0x7D2 (185,285)  card page +
//     0xB08 CCtrlButton nId 0x7D3 (270,285)  list page -     <- the item view's pagers
//     0xB10 CCtrlButton nId 0x7D4 (407,285)  list page +
//     0xAE8 CCtrlEdit   nId 0x7D5 (49, 30) 120x15, CREATEPARAM+0x24 = -1 (opaque white fill)
//
// CCtrlWnd::CreateCtrl (0x004DFBFE) gives EVERY control a `Shape2D#Vector2D` at ctrl+0x18, links it
// to the parent's vector and finishes with `[vec_vtbl+0x90](x, y, vtEmpty, vtEmpty)`. Counting the
// WzLib headers -- IUnknown 3 + IWzSerialize 2 + IWzShape2D 17 -> IWzVector2D starts at slot 22, and
// its 15th method is raw_RelMove -> slot 36 == 0x90. Confirmed from the other side too: CWnd::HitTest
// (0x009E01E7) reads each child's position through `[vec_vtbl+0x68]` and `[+0x70]`, i.e. get_rx
// (slot 26) and get_ry (slot 28), both two slots apart exactly as IWzShape2D declares them. So the
// typed IWzVector2D interface in external/WzLib IS this client's Vector2D ABI, and RelMove-ing
// ctrl+0x18 is precisely what the client itself does to place a control.
//
// And the caret follows it: CCtrlEdit's caret pass (0x004CAF93) computes its x from
// `get_rx(ctrl+0x18) + [ctrl+0x38] - [ctrl+0x60]` at 0x004CAFDD..0x004CAFF0. Round 4's claim that
// "the caret does not come out of that function so it stayed where the control was created" is
// therefore WRONG -- what broke round 4 was that it QUEUED the move to the next main-thread flush,
// so at the instant of the click the edit was still on the other row and CWnd::HitTest found no
// control under the point. Moving it SYNCHRONOUSLY inside the mouse hook (6.G G0.1) fixes both.
//
// CUIMonsterBook::OnMouseButton (0x00862184) head:
//     ecx = [book+0xAE8]; if (!ecx || !ecx->vt[6](rx,ry)) { [book+0x64]=0; SetFocus(book+4); }
// `vt[6]` is CCtrlWnd::IsHit (0x004DFECE), which tests the CONTROL-LOCAL rect {0,0,w,h} against the
// WINDOW-space rx/ry it is handed -- so it can only ever say yes for a click in the window's top-left
// 120x15, and every other click that reaches the book wipes CWndMan::m_pFocus. That is the stock
// v83 behaviour (the mob field works in game DESPITE it, because a click ON the field is routed to
// the control and never reaches the book). This file therefore never lets a field-row click reach
// the original at all: it handles the row itself and returns.
//     Message routing inside it: `msg-0x201 == 0` -> the card-grid selection path (0x00862345 ->
// 0x00867B61 hit test -> 0x0086793F SetSelectedCard). So a card is picked on WM_LBUTTONDOWN, which
// is the message this file also uses for every one of its own SINGLE clicks (6.G G0.3).
//
// CUIMonsterBook::OnMouseMove (0x008623D0), the tooltip ground truth. `this` is the book+4
// sub-object, so every offset in it reads 4 lower than the book's. It bails unless
// `[this+0xADC]->[+0x34] == 2` (content tab == Dropping), walks 20 rects from `this+0xCB8`
// (== book+0xCBC) translated by (+240,+20) with SetRect/PtInRect, resolves the hovered record
// through book+0xE38, takes its `+0x1C` as the item id and calls
//     0x008F5B20(book+0x5C0, GetAbsLeft()+x, GetAbsTop()+y+0x14, itemId, pSlotBlob, 0,0,0,0)
// with `pSlotBlob` built by 0x00483EED -- which only zeroes +0x00..+0x1C and +0x28 and whose dtor
// (0x00483F18) is a no-op while +0x28 is 0. That is exactly the call this file makes with OUR item
// id while the item view is up (6.G G0.6), and the original is not called at all in that state --
// which is why the tester kept seeing the underlying mob's drop tooltip.
//
// GetSlotCard (0x00867A5B) is still the whole result-view trick and is UNCHANGED from round 5,
// because round 5 proved it works in game: remapping (tab,page,index) onto another (tab,page,index)
// the book itself owns gives real, refcounted records, real card art, a real counter and a real
// SetSelectedCard on click. `tab == 9` is the original's own "no card here" answer, so blanking the
// grid fabricates nothing.
//
// -------------------------------------------------------------------------------------------------
// HOOK OWNERSHIP -- three modules share this window; nothing here is attached twice
// -------------------------------------------------------------------------------------------------
//   monsterBookDrops.cpp   Detours on 0x00861E5C Update, 0x00865782 Draw, 0x00866C2C SetMobInfo
//   monsterBookFoundIn.cpp Detours on 0x00685B79 GetInfo, 0x00862184 OnMouseButton
//   THIS FILE              Detours on 0x00867A5B GetSlotCard, 0x00862009 OnButtonClicked,
//                          0x00863DF1 Redraw(pane 0)   -- all three unclaimed by anyone else
//                          plus five VTABLE slots in CUIMonsterBook's OWN private tables, so no
//                          other window is touched and calling the raw address from a hook body
//                          still runs the other module's detour and then the original.
// =================================================================================================

#if USE_MONSTER_BOOK_SEARCH

namespace {

// =================================================================================================
// verified client addresses
// =================================================================================================
constexpr uintptr_t kAddr_Update = 0x00861E5C;          // primary[0]  __thiscall void ()
constexpr uintptr_t kAddr_OnCreate = 0x00861E99;        // primary[3]  __thiscall void (void*) ret 4
constexpr uintptr_t kAddr_OnDestroy = 0x00861F79;       // primary[4]  __thiscall void ()
constexpr uintptr_t kAddr_OnButtonClicked = 0x00862009; // primary[8]  __thiscall void (nId) ret 4
constexpr uintptr_t kAddr_WndDrawThunk = 0x00861E8D;    // primary[11] push [esp+4]; CWnd::Draw; ret 4
constexpr uintptr_t kAddr_OnMouseButton = 0x00862184;   // IUIMsgHandler[2] (msg,wParam,rx,ry) ret 0x10
constexpr uintptr_t kAddr_OnMouseMove = 0x008623D0;     // IUIMsgHandler[3] int (x,y) ret 8
constexpr uintptr_t kAddr_GetSlotCard = 0x00867A5B;     // __thiscall void*(out,tab,page,index) ret 0x10
constexpr uintptr_t kAddr_RedrawPane0 = 0x00863DF1;     // pane dispatcher 0x00866A9E is its ONLY caller
constexpr uintptr_t kAddr_RefreshCtrls = 0x00863C49;    // __thiscall void () ret 0, no stack args

// J-C -- `CWnd::GetCanvas`. THE canvas the window shows, straight from the engine.
//     IWzCanvasPtr* __thiscall CWnd::GetCanvas(CWnd* this, IWzCanvasPtr* out)      ret 4
// Body (0x00425C4C..0x00425D07), read instruction by instruction:
//     cmp [esi+0x20], 0 ; jne 0x00425CAA          ; m_pOverlabLayer wins when it exists
//     (null)  cmp [esi+0x18], 0 ; throw E_POINTER if that is null too, then
//             ecx = [esi+0x18]                    ; m_pLayer
//     (0xCAA) ecx = [esi+0x20]
//     both:   push &Ztl_variant_t(0, VT_I4)  <- built by 0x00402FAB at 0x00425C68 / 0x00425CAA
//             push out ; call 0x00425D2E
// and 0x00425D2E is `layer->raw_get_canvas(VARIANT, IWzCanvas**)` through the layer vtable's
// `[vt+0x100]` slot (0x00425D4E), storing the AddRef'd result straight into `*out` (0x00425D6C).
//
// THIS IS THE ROUND-9 GHOST, and it is the ONE difference between the two canvases: the engine asks
// its layer for canvas **index 0** (a VT_I4 zero), while round 8's hand-rolled `WindowCanvas()`
// asked with **vtEmpty**. `CWnd::Draw` (0x009E0502) blits `backgrnd` (+0x68) at (+0x40,+0x44) into
// whatever THIS function returns (0x009E0552 `call 0x00425C4C`, 0x009E0587 `call [edx+0x80]`), so
// "the canvas the screen shows" is by definition this one -- and a module painting into a
// differently-indexed canvas of the same layer produces exactly the reported symptom: the log says
// the blit succeeded, and the pixels on screen are somebody else's, stale.
constexpr uintptr_t kAddr_CWnd_GetCanvas = 0x00425C4C;

constexpr uintptr_t kAddr_ToolTip_Clear = 0x008E6E23;     // CUIToolTip::ClearToolTip
constexpr uintptr_t kAddr_ToolTip_SetItem = 0x008F5B20;   // ShowItemToolTip(absX, absY, GW_ItemSlotBase*, param, 0,0,0,0) ret 0x20
constexpr uintptr_t kAddr_ToolTip_Ctor = 0x008E49B5;      // CUIToolTip::CUIToolTip() -- worldMapInfo.cpp
constexpr uintptr_t kAddr_ToolTip_SetString2 = 0x008E7150; // see wvs/tooltip.h SetToolTip_String2
constexpr uintptr_t kAddr_GetAbsLeft = 0x009E03C5;        // both __thiscall on the book+4 sub-object
constexpr uintptr_t kAddr_GetAbsTop = 0x009E0447;
// CUIToolTip members we read back to PROVE a tooltip was actually built (wvs/tooltip.h).
constexpr uint32_t kOff_Tip_Height = 0x08;
constexpr uint32_t kOff_Tip_Width = 0x0C;
// IUIMsgHandler vtable slots, byte-verified against 0x00B39FE8: [11] 0x009E03C5, [12] 0x009E0447.
// The stock hover reaches them through `call [eax+0x2c]` / `[eax+0x30]` (0x008624D4 / 0x008624E3),
// so going through the table is exactly what the client does -- and it cannot go stale.
constexpr int kVTIdx_GetAbsLeft = 11;
constexpr int kVTIdx_GetAbsTop = 12;

// `CCtrlTab::SetSelected(int)` -- what SetSelectedCard itself calls to move the colour strip
// (0x00867970: `mov ecx,[esi+0xad8]` then `push tab / call 0x8603df`). Bounds-checked internally
// against the tab array at ctrl+0x44, so an out-of-range index is refused rather than fatal.
constexpr uintptr_t kAddr_CCtrlTab_SetSelected = 0x008603DF;
// `CUIMonsterBook::SetCardPage(int)` -- zeroes +0x5B8 / +0x5B4, stores +0x5B0 and dirties panes
// 0, 1 and 2 (0x00866AC3, read instruction by instruction). Used instead of a raw store so the
// selection and the list page are reset exactly the way the client resets them.
constexpr uintptr_t kAddr_Book_SetCardPage = 0x00866AC3;
// `CUIMonsterBook::SelectCurrentSlot()` -- the client's own "select whatever the grid's current
// (tab, +0x5B0, +0x5B8) is". K8 calls it so a result view opens on its first result.
//
// ABI PROVEN, not read off a call site (the J-A lesson). 0x00867923 disassembles to:
//     push ecx / push esi / mov esi,ecx / push ecx / push ecx      ; 8-byte stack `out`
//     mov eax,esp / mov [esp+0xc],esp / push eax
//     call 0x00867A2F                                             ; GetSelectedCard(out), ret 4
//     mov ecx,esi / call 0x0086793F                               ; SetSelectedCard(out.record)
//     pop esi / pop ecx / ret                                     ; <- ret 0, NO stack args
// so it is `__thiscall void ()` on the BOOK. `GetSelectedCard` (0x00867A2F) is what makes it useful
// here: it pushes +0x5B8 (index), +0x5B0 (page) and `[+0xAD8]+0x34` (tab) into GetSlotCard -- i.e.
// straight through THIS module's remap -- and hands the answer to `SetSelectedCard`, which does the
// rest of the client's own selection (left tab to record+8, right tab to 0, SetMobInfo).
constexpr uintptr_t kAddr_Book_SelectCurrentSlot = 0x00867923;
// `GetCardLevel(int cardId)` __cdecl. monsterBook.cpp DETOURS it for the dual-key fix, so calling
// the raw address here runs that detour first and the login-snapshot key works for free (H1.10).
constexpr uintptr_t kAddr_GetCardLevel = 0x0095FC65;

// -------------------------------------------------------------------------------------------------
// I1.3 -- the Dropping row's item id lives behind a ZtlSecureFuse<int>, NOT in a plain field.
//
// Re-derived for round 8 from the two client paths that own that list, and they agree:
//
//   THE BUILDER (0x0086743A..0x0086747B), per row:
//       eax = record->reward[i]                  ; the item id straight out of CMonsterBookMan
//       ecx = row + 0x0C ;  call 0x004287F2      ; ZtlSecureFuse<int>::Set(row+0x0C, itemId)
//       ecx = row + 0x0C ;  call 0x0042873D      ; ...Get, immediately read back
//       call 0x005D5D95                          ; NOT GetItemName -- see the J-A block below
//       ecx = row + 0x18 ;  call 0x00428285      ; ZRef::operator=  -- an ITEM SLOT lands at row+0x18
//
// ROUND-9 CORRECTION (J-A). Round 8's reading of that fourth line was wrong and it is what crashed
// the client. `0x005D5D95` is **not** `CItemInfo::GetItemName`; it is the item-slot factory
// (call it `GetItemSlot`), and its `out` parameter is EIGHT bytes, not four. Proof, from the callee
// and from three real call sites:
//   * callee 0x005D5D95: `__thiscall`, two stack args, `ret 8` (epilogue 0x005D6385..0x005D6393),
//     returns `out` in eax. `[ebp+0x0C]` is the item id -- it is divided by 0xF4240 and dispatched
//     1 = equip / 2..4 = bundle / 5 = cash (0x005D5DA7..0x005D5DD7), each branch ZAllocEx-ing an
//     object (0xF9 bytes for equip @0x005D6052, 0x4D for a bundle @0x005D5FBD), stamping a vtable
//     (0xAF310C / 0xAF318C), pushing the id through `ZtlSecureFuse<int>::Set` at object+0x0C and
//     setting quantity 1 at object+0x28.
//   * it then writes that object to **`out+4`** (0x005D5E6A, 0x005D6020, 0x005D6374) and AddRefs it
//     via `object+4` (`call [0xAF01FC]`); the not-found path writes `out+4 = 0` (0x005D6047).
//     `out+0` is never touched.
//   * call site 0x0086746B (this very builder): `out` is `[ebp-0x34]` and the cleanup two
//     instructions later reads `[ebp-0x30]` -- i.e. out+4 -- and passes `out` to `0x00428A50`
//     (Release: `InterlockedDecrement(p+4)`, free at 0x00402D2E). Call site 0x004808CE reads the
//     RESULT's `+4` the same way (`cmp [eax+4], ebx / sete cl`). Call site 0x00424ADD destroys
//     `out` with `0x004288C1`, whose whole body is `if (this->[+4]) { Release; [+4] = 0; }`.
// So handing that function a 4-byte `ZXString<char>` -- which is what round 8 did -- makes it store
// a pointer 4 bytes PAST the variable. That is precisely "Run-Time Check Failure #2 -- Stack around
// the variable 'out' was corrupted", on every path that resolves an item name, which is the reported
// crash. `catch(...)` cannot see an RTC epilogue check, so the only fix is the correct signature --
// and the correct FUNCTION, which is `0x005CF63E` (see kAddr_CItemInfo_GetItemName below).
//
//   THE DRAW (0x00865D50..0x00865D8B), per icon:
//       push [rect+0x0C] / push [rect+0]         ; the slot rect the icon goes in
//       call 0x00867D34                          ; -> the row object
//       ecx = row + 0x0C ;  call 0x00428229      ; == `jmp 0x0042873D`, the same Get
//       ... call 0x005D6458                      ; DrawItemIconForSlot with THAT id
//
// So the id the player SEES under the cursor is `Get(row+0x0C)`, full stop. Round 7 read row+0x1C
// plain -- which the STOCK HOVER also does (0x008624C0) -- and got an id the server had no droppers
// for, which is why every Dropping click came back with an empty grid. row+0x18 is the name string,
// so row+0x1C is four bytes INTO it; the stock hover has always been feeding the tooltip garbage
// there, and nobody noticed because the item view suppressed it.
//
// `Get` is not a pure read: it bumps a global at 0x00BEDDBC and re-encodes the blob every 55th call
// (0x004287C3 `push 0x37` / `idiv`), and on a checksum mismatch it does
// `mov [ebp-0x10],5 / call 0x00A60BB7` -- _CxxThrowException, i.e. the ZException(5) the spear-stab
// investigation already met. Hence the two-layer guard below: C++ catch(...) for the throw, SEH
// around it for a hard fault, and the WZ `reward` ordinal as the fallback either way.
constexpr uintptr_t kAddr_ZtlSecureFuse_Get = 0x0042873D;
constexpr uint32_t kOff_DropRec_ItemFuse = 0x0C;

// -------------------------------------------------------------------------------------------------
// J-A -- `CItemInfo::GetItemName`, PROVEN, at the address the rest of Kentakae already uses.
//
//     ZXString<char>* __thiscall CItemInfo::GetItemName(CItemInfo* this, ZXString<char>* out,
//                                                       int nItemID)          ret 8
//
// Callee, 0x005CF63E read instruction by instruction:
//     0x005CF63E  mov eax, 0xA93B6B / call 0xA60B98      ; __EH_prolog3, so [ebp+8] is arg 1
//     0x005CF659  mov edi, ecx                            ; `this`
//     0x005CF653  push 0x644 / call 0x0079E805            ; StringPool[0x644] == "name"
//     0x005CF674  push [ebp+8] (out) / push [ebp+0xC] (itemId) / push "name" / ecx = this
//                 call 0x005CF6FE                         ; the shared worker
//     0x005CF68E  mov eax, [ebp+8]                        ; returns `out`
//     0x005CF69B  ret 8                                   ; two stack args, callee-cleaned
// The worker (0x005CF6FE) writes through that pointer exactly ONCE and exactly 4 bytes:
// `mov edi,[ebp+8] / mov [edi], ebx(0) / call 0x004181C9(edi, &tmp)` -- and 0x004181C9 is
// `ZXString<char>::operator=`, whose only store is `[esi]`. So `out` is a plain 4-byte ZXString
// handle, and 0x005CF69E is the identical function for "desc" (StringPool[0x5A7]).
//
// The RELEASE is the caller's job and round 8 never did it. Byte-for-byte, from call site
// 0x00424859..0x0042486C:
//     mov eax, [ebp-0x14]        ; the returned char*
//     cmp eax, 0 / je skip
//     add eax, -0x0C             ; the _ZXStringData header is 12 bytes BEFORE the characters
//     push eax / call 0x00428D13 / pop ecx        ; __cdecl release
// and 0x00428D13 is `InterlockedDecrement(pHeader); if (<= 0) ZAllocEx(0x00BF0A90)::Free(pHeader);`
// -- the same pool `ZALLOCEX(ZAllocStrSelector<char>, 0x00BF0A90)` in core/injector.cpp binds, so
// Kentakae's own `ZXString<char>::~ZXString` would free identically. The wrapper below nevertheless
// uses the raw slot + the client's own release call, so it cannot depend on that equivalence.
//
// Cross-checked against the two other Kentakae modules that already ship this call:
// `features/equip/androidSlots.cpp` (which DETOURS 0x005CF63E) and `features/ui/dailyCheckin.cpp`.
constexpr uintptr_t kAddr_CItemInfo_GetItemName = 0x005CF63E;
// `ZXString<char>::_Release(_ZXStringData*)`, __cdecl, `ret` with no immediate (caller pops).
constexpr uintptr_t kAddr_ZXString_Release = 0x00428D13;
constexpr int kOff_ZXStringHeader = 12; // characters - 12 == the {nRef, nCap, nByteLen} header
// CItemInfo::GetItemSlot(ZRef<GW_ItemSlotBase>* out, long itemId), ret 8. NOT a name getter: `out` is an
// 8-byte ZRef (payload at +4). Used only to build the tooltip's item slot (SetItemTipOn), exactly as
// cashshopwnd.cpp does; released through ZRef<GW_ItemSlotBase>::~ZRef 0x00428A50 (ret 4, no null check).
constexpr uintptr_t kAddr_CItemInfo_GetItemSlot = 0x005D5D95;
constexpr uintptr_t kAddr_ZRefItemSlot_Release = 0x00428A50;
constexpr uintptr_t kAddr_CItemInfo_Instance = 0x00BE78D8;

constexpr uintptr_t kAddr_CWndMan_SetFocus = 0x009E3264; // __thiscall void (IUIMsgHandler* pCtrlSub)
constexpr uintptr_t kAddr_CWndMan_Instance = 0x00BEC20C;
constexpr uint32_t kOff_WndMan_Focus = 0x88;
// J-B round 10 -- the WINDOW BEING DRAGGED (a CWnd*, not a sub-object). Written at 0x009E3F76 when a
// 0x201 lands on a window whose HitTest answered 1 (the caption band), tested every mouse message at
// 0x009E3B55, and zeroed at 0x009E3CA5 the instant the left button is found up (0x0059A25A is
// `IsKeyDown(vk)`, called with 1 = VK_LBUTTON). Distinct from +0x88 (focus) and +0x8C (the last
// mouse-over window), both of which this same function writes a few instructions away.
constexpr uint32_t kOff_WndMan_Drag = 0x84;

constexpr uintptr_t kAddr_ZAlloc_Alloc = 0x00403065;     // __thiscall void* Alloc(size)
constexpr uintptr_t kAddr_ZAlloc_Instance = 0x00BF0B00;  // ZAllocEx<...>::_s_alloc
constexpr uintptr_t kAddr_CCtrlButton_Ctor = 0x004258E4; // __thiscall CCtrlButton::CCtrlButton()
constexpr unsigned kSize_CCtrlButton = 0x5A4;            // `mov ebx,0x5A4` @0x008626BA

// `or dword ptr [ebp-0x48], 0FFFFFFFFh` -- the stock edit's CREATEPARAM+0x24 back colour. Turning
// it into `and dword ptr [ebp-0x48], 0` (same 4 bytes) makes the edit paint NO fill, because
// CCtrlEdit::Draw skips its DrawRectangle when the colour is 0 (`cmp [esi+0x80],0 / je` @0x004CA784).
// That is what lets the baked box art -- and its grey placeholder -- show through.
constexpr uintptr_t kAddr_StockEditFill = 0x00862978;

// vtable slots, every one dumped out of .rdata for this round:
//   0x00B39FE8 IUIMsgHandler (19 entries)          0x00B3A034 primary (0x00B39FE8 + 19*4)
//     [2] 0x00B39FF0 = 0x00862184 OnMouseButton      [ 0] 0x00B3A034 = 0x00861E5C Update
//     [3] 0x00B39FF4 = 0x008623D0 OnMouseMove        [ 3] 0x00B3A040 = 0x00861E99 OnCreate
//                                                    [ 4] 0x00B3A044 = 0x00861F79 OnDestroy
//                                                    [11] 0x00B3A060 = 0x00861E8D CWnd::Draw thunk
constexpr uintptr_t kVTSlot_Update = 0x00B3A034 + 0 * 4;
constexpr uintptr_t kVTSlot_OnCreate = 0x00B3A034 + 3 * 4;
constexpr uintptr_t kVTSlot_OnDestroy = 0x00B3A034 + 4 * 4;
constexpr uintptr_t kVTSlot_WndDraw = 0x00B3A034 + 11 * 4;
constexpr uintptr_t kVTSlot_OnMouseButton = 0x00B39FE8 + 2 * 4;
constexpr uintptr_t kVTSlot_OnMouseMove = 0x00B39FE8 + 3 * 4;

// =================================================================================================
// J-A, second half -- THE ABI SWEEP. Every raw engine call this file makes, re-proved against its
// own callee for round 9 (entry prologue + an intra-procedural CFG walk for the real `ret n` + the
// highest `[ebp+arg]` / `[esp+arg]` slot the body reads). "wrong ABI guessed from a call site" is
// the failure class that produced the crash, so no entry here is inherited on trust.
//
//   address     what                              proven                       declared here
//   ----------  --------------------------------  ---------------------------  ---------------
//   0x005CF63E  CItemInfo::GetItemName            ret 8,  ecx + 2 args         MATCHES (rewritten)
//   0x005D5D95  CItemInfo::GetItemSlot            ret 8,  ecx + 2 args, out    NOT CALLED any more
//                                                 is 8 BYTES (payload at +4)   -- this was the bug
//   0x00428D13  ZXString<char>::_Release          ret 0 (cdecl), 1 arg         MATCHES (new)
//   0x0042873D  ZtlSecureFuse<int>::Get           ret 0,  ecx, 0 args          MATCHES
//   0x008F5B20  CUIToolTip::SetItemToolTip        ret 0x20, ecx + 8 args       MATCHES
//   0x008E7150  CUIToolTip::SetToolTip_String2    ret 0x28, ecx + 10 args      MATCHES (= wvs/tooltip.h)
//   0x008E6E23  CUIToolTip::ClearToolTip          ret 0,  ecx, 0 args          MATCHES
//   0x008E49B5  CUIToolTip::CUIToolTip            ret 0,  ecx, 0 args          MATCHES
//   0x00483EED  ItemSlotTemp ctor                 ret 0,  ecx, 0 args          MATCHES
//   0x00483F18  ItemSlotTemp dtor                 ret 0,  ecx, 0 args          MATCHES
//   0x009E03C5  IUIMsgHandler::GetAbsLeft  [11]   ret 0,  ecx, 0 args          MATCHES
//   0x009E0447  IUIMsgHandler::GetAbsTop   [12]   ret 0,  ecx, 0 args          MATCHES
//   0x00425C4C  CWnd::GetCanvas                   ret 4,  ecx + 1 arg          MATCHES (new, J-C)
//   0x00863C49  CUIMonsterBook::RefreshCtrls      ret 0,  ecx, 0 args          MATCHES
//   0x008603DF  CCtrlTab::SetSelected             ret 4,  ecx + 1 arg          MATCHES
//   0x00866AC3  CUIMonsterBook::SetCardPage       ret 4,  ecx + 1 arg [esp+4]  MATCHES
//   0x0095FC65  GetCardLevel                      ret 0 -> __cdecl, 1 arg      MATCHES
//   0x00403065  ZAllocEx::Alloc                   ret 4,  ecx + 1 arg          MATCHES; and `this`
//                                                                              really is the OBJECT
//                                                                              AT 0x00BF0B00
//                                                                              (`mov ecx,0xBF0B00`
//                                                                              @0x005D5E09), not a
//                                                                              pointer read
//   0x004258E4  CCtrlButton::CCtrlButton          ret 0,  ecx, 0 args          MATCHES
//   0x004BFFFB  CCtrlButton::CreateCtrl (vt[8])   ret 0x18, ecx + 6 args       MATCHES; slot 8 of
//                                                                              CCtrlButton's own
//                                                                              vtable 0x00AF0C10
//                                                                              (`mov [esi],0xAF0C10`
//                                                                              @0x0042593F) really
//                                                                              is 0x004BFFFB
//   0x004E0232  CCtrlWnd::SetShow  (sub vt[9])    ret 4,  ecx + 1 arg          MATCHES
//   0x004E01F6  CCtrlWnd::SetEnable(sub vt[7])    ret 4,  ecx + 1 arg          MATCHES
//   0x004CA86B  CCtrlEdit::SetEnable (override)   ret 4,  ecx + 1 arg          same shape; never
//                                                                              called on an edit
//   0x009E3264  CWndMan::SetFocus                 ret 4,  ecx + 1 arg          MATCHES
//   0x0049637B  CClientSocket::SendPacket         ret 4,  ecx + 1 arg          MATCHES
//   0x005D6458  CItemInfo::DrawItemIconForSlot    ret 0x28, ecx + 10 args      MATCHES (wvs/iteminfo.h)
//   0x00BE78D8  CItemInfo::ms_pInstance           POINTER (`mov edi,[0xBE78D8]`  MATCHES -- both the
//                                                 @0x00867456)                  raw deref here and
//                                                                               TSingleton::GetInstance
//   0x00BEC20C / 0x00BE7914 / 0x00BEBF98          pointer reads                MATCHES
// The vtable-dispatched originals (Update / OnCreate / OnDestroy / CWnd::Draw thunk / OnMouseButton
// / OnMouseMove / GetSlotCard / OnButtonClicked / Redraw(0)) are unchanged from round 8 and are
// re-entered through the exact slot the engine would have used, so their ABI is the engine's own.
// =================================================================================================

constexpr uintptr_t kAddr_CClientSocket_SendPacket = 0x0049637B;
constexpr uintptr_t kAddr_CClientSocket_Instance = 0x00BE7914;
constexpr uintptr_t kAddr_CUserLocal_Instance = 0x00BEBF98;
constexpr uint32_t kOff_CUserLocal_CharId = 0x11A8; // CUser::GetCharacterId 0x007A6E53 `mov eax,[ecx+0x11A8]` (damageskin.cpp); was 0x19E8, unused in this exe
constexpr uint32_t kOff_CClientSocket_Socket = 0x08;
constexpr uint32_t kOff_CClientSocket_Closing = 0x14;

// =================================================================================================
// CUIMonsterBook layout
// =================================================================================================
constexpr uint32_t kOff_WndLayer = 0x18;       // CWnd::m_pLayer        (CWnd::GetCanvas 0x00425C88)
constexpr uint32_t kOff_WndOverlab = 0x20;     // CWnd::m_pOverlabLayer (preferred, 0x00425C5F)
constexpr uint32_t kOff_BgDstLeft = 0x40;      // where CWnd::Draw blits `backgrnd` (0x009E056D)
constexpr uint32_t kOff_BgDstTop = 0x44;
constexpr uint32_t kOff_WndBackgrnd = 0x68;    // the `backgrnd` canvas CWnd::Draw copies (0x009E0515)
constexpr uint32_t kOff_CardPage = 0x5B0;      // card-grid page       (0x7D1 / 0x7D2)
constexpr uint32_t kOff_ListPage = 0x5B4;      // right-page list page (0x7D3 / 0x7D4)
constexpr uint32_t kOff_SelSlot = 0x5B8;       // selected slot inside the card page (0x00867A2F)
constexpr uint32_t kOff_ToolTip = 0x5C0;       // CUIToolTip sub-object
constexpr uint32_t kOff_LeftTabCtrl = 0xAD8;   // colour tabs; selected index at +0x34, 9 = cover
constexpr uint32_t kOff_RightTabCtrl = 0xAE0;  // 0 Basic Info, 1 Episode, 2 Dropping, 3 Found In
constexpr uint32_t kOff_TabSelected = 0x34;
constexpr uint32_t kOff_EditCtrl = 0xAE8;      // CCtrlEdit* (ZRef at 0xAE4)
constexpr uint32_t kOff_BtSearch = 0xAF0;      // CCtrlButton* nId 0x7D0
constexpr uint32_t kOff_ArrowCardPrev = 0xAF8; // nId 0x7D1
constexpr uint32_t kOff_ArrowCardNext = 0xB00; // nId 0x7D2
constexpr uint32_t kOff_ArrowListPrev = 0xB08; // nId 0x7D3
constexpr uint32_t kOff_ArrowListNext = 0xB10; // nId 0x7D4
constexpr uint32_t kOff_PaneDirty = 0xB14;     // dirty[3], stride 8 (Update 0x00861E63)
constexpr uint32_t kOff_LeftPageLayer = 0xB18; // IWzGr2DLayer* of the LEFT page, window (40,25)
constexpr uint32_t kOff_PageLayer = 0xB20;     // IWzGr2DLayer* of the RIGHT page, window (240,20)
constexpr uint32_t kOff_CardRects = 0xB2C;     // RECT[25], card grid, canvas space (+40,+25)
constexpr uint32_t kOff_DropRects = 0xCBC;     // RECT[20], Dropping grid, canvas space (+240,+20)
constexpr uint32_t kOff_TabPages = 0xDFC;      // void** per colour tab, tab*4
constexpr uint32_t kOff_MobName = 0xE24;       // ZXString<char>, centred on the right page by Draw
// The DROPPING list the client itself built: array of pages, each page an array of {key,value}
// pairs (stride 8, record at +4), the record's item id at +0x1C. This is the exact walk the stock
// hover performs at 0x0086246A..0x008624C0 with `esi == book+4`, i.e. `[esi+0xE34] == book+0xE38`.
constexpr uint32_t kOff_DropList = 0xE38;
// record+0x18 is a ZRef<GW_ItemSlotBase> (filled by ZRef::operator= at 0x0086747B), so +0x1C is the
// item object pointer, never an id. The id comes from the fuse at +0x0C only.

// CCtrlWnd / CCtrlEdit internals, from CreateCtrl (0x004DFBFE), CCtrlEdit::Draw (0x004CA700) and
// the caret pass (0x004CAF93).
constexpr uint32_t kOff_Ctrl_Id = 0x14;        // nId              (0x004DFC16)
constexpr uint32_t kOff_Ctrl_Vector = 0x18;    // IWzVector2D*     (0x004DFC39)
constexpr uint32_t kOff_Ctrl_Width = 0x1C;     // w                (0x004DFD8E)
constexpr uint32_t kOff_Ctrl_Height = 0x20;    // h                (0x004DFD94)
constexpr uint32_t kOff_Ctrl_Parent = 0x24;    // CWnd*            (0x004DFC22)
constexpr uint32_t kOff_Ctrl_Enabled = 0x2C;   // SetEnable writes sub+0x28 == ctrl+0x2C
constexpr uint32_t kOff_Ctrl_Shown = 0x30;     // SetShow   writes sub+0x2C == ctrl+0x30
constexpr uint32_t kOff_Ctrl_Text = 0x34;      // ZXString<char>   (GetText 0x00471362)
constexpr uint32_t kOff_Ctrl_TextInsetX = 0x38;
constexpr uint32_t kOff_Ctrl_TextInsetY = 0x3C;
constexpr uint32_t kOff_Ctrl_Caret = 0x48;     // caret index      (0x004CB792 / 0x004CB797)
constexpr uint32_t kOff_Ctrl_SelAnchor = 0x5C; // selection anchor, -1 = none (0x004CB6E9)
constexpr uint32_t kOff_Ctrl_HScroll = 0x60;   // horizontal scroll (0x004CAFEA)
constexpr uint32_t kOff_Ctrl_BackColor = 0x80; // 0 = no fill      (0x004CA784)

// -------------------------------------------------------------------------------------------------
// ROUND 11 -- THE CARET, READ OUT OF THE CLIENT INSTEAD OF GUESSED (6.K-K3.3, and the row-1 half of
// this round's brief). Everything below was disassembled for this round; nothing is inferred.
//
// `CCtrlEdit::DrawTextRun` (0x004CAF93, the function 6.J called "the caret pass") is the TEXT pass,
// not a caret pass. Read instruction by instruction it is:
//     0x004CAFDD  ecx = [esi+0x18] ; call 0x00425AEF          ; get_rx(vector)
//     0x004CAFE7  eax = [esi+0x38] - [esi+0x60] ; ebx += eax  ; + insetX - hScroll   -> the TEXT X
//     0x004CAFF8  call 0x004E0101                             ; CWndMan[0xBEC20C]+0x88 == this+4 ?
//                                                             ;   i.e. "am I the focus owner"
//     0x004CB093 / 0x004CB2C2  `add ebx,[ebp-0x18]`           ; ebx advances by each segment's
//                                                             ;   measured width (0x0042782E)
// so the x of character N is exactly `textX + width(text[0..N))`, and `[ctrl+0x48]` is N: the key
// handler at 0x004CB76D writes `strlen([ctrl+0x34])` into it for End (0x004CB78B..0x004CB792), `0`
// for Home (0x004CB797), and steps it by CharNextA / CharPrevA (0x004CB7EE / 0x004CB7C8) for the
// arrows; `CCtrlEdit::SetText` (0x004CC512) sets it to the new length.
//
// The caret ITSELF is a separate little object embedded at `ctrl+0x94`. `CCtrlEdit`'s per-frame
// state pass (0x004CA22D) is the whole binding:
//     mov eax,[esi+4] ; lea ecx,[esi+4] ; call [eax+0x28]     ; IsShown
//     mov ecx,esi     ; call 0x004E0101                       ; IsFocused
//     push (shown && focused) ; lea ecx,[esi+0x94] ; call 0x004C932A
// and 0x004C932A is `CCaret::SetVisible(BOOL)`:
//     if (!arg) goto hide;
//     eax = <tick>() [0x00BF060C] ; eax -= [this+8] ; edx:eax / 0x12C   <- 300 ms
//     if (al & 1) goto hide;                                            <- odd bucket = invisible
//     show: colour = 0xFFFFFFFF   hide: colour = 0x00FFFFFF (alpha 0)
//     [this+0x14]->vt[0xE0](colour)
// with `[caret+0x14]` the 1 x [caret+0x10] white bar built at 0x004C8E69..0x004C8F12
// (`raw_DrawRectangle(0, 0, 1, height, -1)`).
//
// So: half-period 300 ms, phase anchored at `[caret+8]`, and the caret is only alive while the edit
// is shown AND owns the focus. The module reproduces exactly that for the row whose box covers the
// control's own output -- see PaintRow's caret block.
constexpr uint32_t kOff_Ctrl_SelRectY = 0x44;  // CREATEPARAM+0x10; selection/caret y = get_ry + this
constexpr uint32_t kOff_Ctrl_LineH = 0x7C;     // CREATEPARAM+0x18; the highlight/caret height (12)
constexpr uint32_t kOff_Ctrl_Caret_Obj = 0x94; // the embedded CCaret (0x004CA250 `lea ecx,[esi+0x94]`)
constexpr uint32_t kOff_Caret_PhaseTick = 0x08; // 0x004C933A `sub eax,[esi+8]`
constexpr DWORD kCaretBlinkHalfMs = 300;       // 0x004C933F `mov ecx,0x12C`
constexpr unsigned kCaretColor = 0xFF000000;   // the edit's own text colour, CREATEPARAM+0x1C
constexpr int kCaretW = 1;                     // the bar the client builds is one pixel wide

// -------------------------------------------------------------------------------------------------
// ROUND 12 -- WHERE THE CARET'S POSITION ACTUALLY LIVES (6.K-K3.3, closed).
//
// Rounds 7 and 11 both looked for the caret's position inside the DRAW passes and found only the
// control vector, which demonstrably moves. It is not in a draw pass at all. `CCaret` owns its own
// Gr2D layer and a pair of BAKED BASE COORDINATES, and every position it is ever given is an OFFSET
// FROM THOSE. The three functions, disassembled for this round:
//
//   `CCaret::CCaret` 0x004C8DF9   (thiscall, `ret 0x14`: parentWnd, baseX, baseY, height, z)
//       0x004C8E10  call [0x00BF060C]        ; GetTickCount
//       0x004C8E16  mov  [ebx+8], eax        ; <- the blink phase anchor this module already reads
//       0x004C8E19  mov  eax,[ebp+0x0C] ; mov [ebx+0x00], eax     ; <-- caret BASE X   ** baked **
//       0x004C8E1E  mov  eax,[ebp+0x10] ; mov [ebx+0x04], eax     ; <-- caret BASE Y   ** baked **
//       0x004C8E24  mov  eax,[ebp+0x14] ; mov [ebx+0x10], eax     ; the bar's height (12)
//       0x004C8E77  call 0x00426C7E          ; CreateLayer(0, 0, 1, height, z, ...) -> [ebx+0x14]
//       0x004C8F12  call [ecx+0x8C]          ; raw_DrawRectangle(0, 0, 1, height, -1) -- the bar
//       0x004C9030  push [ebx+4] ; push [ebx] ; call 0x00432C2D   ; layer.origin->RelMove(baseX, baseY)
//       0x004C90A2  mov  [ebx+0x0C], 1       ; "alive" -- SetPos refuses to run while this is 0
//
//   `CCaret::SetPos` 0x004C90E3   (thiscall, `ret 0x0C`: dx, dy, barWidth) -- the ONLY mover
//       0x004C90F7  cmp [ebx+0x0C], 0 ; je <exit>            ; not alive -> no-op
//       0x004C9106  mov [ebx+8], eax                          ; restart the blink at "on"
//       0x004C9120  call [eax+0xE0] with -1                   ; layer colour = opaque white
//       0x004C9185  mov eax,[ebx+4] ; add eax,[ebp+0x0C]      ; y = BASE Y + dy
//       0x004C918B  mov ecx,[ebx+0]  ; add ecx,[ebp+0x08]     ; x = BASE X + dx
//       0x004C91B2  call [edx+0x90]                           ; origin->RelMove(x, y, vt, vt)
//       0x004C9293  call [ecx+0x90]                           ; size->RelMove(barWidth, [ebx+0x10])
//
//   `CCtrlEdit::UpdateCaret` 0x004CB829 (thiscall, `ret 0x0C`) -- SetPos's ONE AND ONLY caller
//       0x004CBA76  measures text[0 .. [ctrl+0x48]) with the control's own font -> [ctrl+0x58]
//       0x004CBAE3  mov [ebp+8], 1                            ; the bar is 1 px wide
//       0x004CBC0B  adjusts the h-scroll [ctrl+0x60] so the caret stays inside the field
//       0x004CBC63  sub eax,[esi+0x60]
//       0x004CBC67  push 0                                    ; <== dy IS THE LITERAL ZERO
//       0x004CBC70  call 0x004C90E3
//
// So the caret's y is `BASE Y + 0` for the entire life of the control, and BASE Y is written exactly
// once, by `CCtrlEdit::CreateCtrl` at 0x004CA575..0x004CA59B:
//       0x004CA572  push [esi+0x7C]                           ; height
//       0x004CA575  lea  edi,[esi+0x18] ; call 0x00432BFA ; call 0x00425B16   ; get_y(ctrl vector)
//       0x004CA586  add  eax,[esi+0x44] ; push eax             ; BASE Y = ctrl.y + [ctrl+0x44]
//       0x004CA58B  lea  ...            ; call 0x00425AEF      ; get_x(ctrl vector)
//       0x004CA598  add  eax,[esi+0x40] ; push eax             ; BASE X = ctrl.x + [ctrl+0x40]
//       0x004CA5B7  call 0x004C8DF9
// (0x00425AEF is `[vec_vt+0x68]` and 0x00425B16 is `[vec_vt+0x70]` -- the SAME two accessors
// `CCtrlEdit::DrawTextRun` uses at 0x004CAFE0, so the caret base is in the same coordinate space the
// module already reads back after a `raw_RelMove`.)
//
// THAT is the defect. The book creates its edit on the MOB row (window y 30); `raw_RelMove` on
// `ctrl+0x18` moves the control, the vector, the glyph pass and the hit test -- but NOT the two
// dwords at `caret+0x00/+0x04`, which keep saying 30 forever. Hence: text on row 0, caret on row 1.
//
// A full-image scan (`lea`/`mov` against +0x94, then every caller of each CCaret method) proves the
// enumeration is complete: `caret+0x00/+0x04` are touched by exactly two instructions in the whole
// executable -- zeroed by `CCtrlEdit::CCtrlEdit` at 0x004CC3D2/0x004CC3D8 and set by the ctor above.
// There is no client setter for them, so this module writes them itself and then hands the placement
// back to the engine via `CCtrlEdit::UpdateCaret`, which is byte-for-byte what `CreateCtrl` does on
// the line after it builds the caret (0x004CA5E4 `mov [esi+0x48],eax` then 0x004CA5E7 `call`).
constexpr uintptr_t kAddr_CCtrlEdit_UpdateCaret = 0x004CB829; // __thiscall (int, int, int), ret 0xC
constexpr uintptr_t kAddr_CCaret_SetPos = 0x004C90E3;         // kept for the record; called via ^^^
constexpr uint32_t kOff_Ctrl_CaretBaseX = 0x40; // CREATEPARAM+0x0C; BASE X = get_x + this (0x004CA598)
constexpr uint32_t kOff_Ctrl_CaretPixelX = 0x58; // measured width of text[0..caretIdx) (0x004CBABB)
constexpr uint32_t kOff_Caret_BaseX = 0x00;     // 0x004C8E1C / read at 0x004C918B
constexpr uint32_t kOff_Caret_BaseY = 0x04;     // 0x004C8E21 / read at 0x004C9185
constexpr uint32_t kOff_Caret_Alive = 0x0C;     // 0x004C90A2 = 1, dtor 0x004CA6F1 = 0
constexpr uint32_t kOff_Caret_BarH = 0x10;      // 0x004C8E30, == [ctrl+0x7C]
constexpr uint32_t kOff_Caret_Layer = 0x14;     // the 1 x BarH white bar

// -------------------------------------------------------------------------------------------------
// ...and the FONT the stock edit actually uses, from the same read.
//
// `CCtrlEdit::CreateCtrl` (0x004CA25D) copies CREATEPARAM+0x14/+0x18/+0x1C into the font it builds
// from `StringPool[0x582]` == "Canvas#Font" (0x004CA336..0x004CA359, then 0x00405210 configures it),
// and the book hands it the DEFAULT CREATEPARAM: `CCtrlEdit::CREATEPARAM::CREATEPARAM` (0x004C8D5F)
// stores `StringPool[0x1597]` == **"Arial"** at +0x14, **0xC == 12** at +0x18 and **0xFF000000** at
// +0x1C (0x004C8D92 / 0x004C8DDE / 0x004C8DE5). The book's own edit factory (0x00862970 ->
// 0x008629CC) overrides exactly ONE field of it, `+0x24 = -1` (the opaque fill) at 0x00862978.
//
// The module was drawing the SAME row's text in Dotum 11, so a stash and the live control rendered
// the same string differently and the row visibly changed shape when the edit moved off it. The row
// text (and the caret metrics) now come from this pair; the level control and the % labels keep the
// Dotum face they were tuned against.
constexpr const wchar_t* kRowFontFace = L"Arial"; // StringPool[0x1597], CREATEPARAM+0x14
constexpr unsigned kRowFontHeight = 12;          // CREATEPARAM+0x18

// The four control-state slots on the ctrl+4 sub-object. Dumped from .rdata for this audit:
//   CCtrlTab  0x00B3A070: [7] 0x004E01F6 SetEnable  [8] 0x004259DD  [9] 0x004E0232 SetShow  [10] 0x004259E1
//   CCtrlEdit 0x00AF2B5C: [7] 0x004CA86B SetEnable  [8] 0x004259DD  [9] 0x004E0232 SetShow  [10] 0x004259E1
// CORRECTION (round-6 audit): slot 7 is NOT shared -- CCtrlEdit OVERRIDES it (0x004CA86B, not the
// base 0x004E01F6). Slot 9 IS the same function in both, and slot 9 is the only one this file ever
// calls on an edit or a tab, so nothing here depends on the retracted claim. SetEnable is only
// ever used on the four pager BUTTONS, whose slot 7 is the base -- proven by RefreshCtrls calling
// `[eax+0x1C]` on exactly those four. Only SetEnable(0) can drop focus (0x004E0221), which is why
// nothing here ever disables a field.
constexpr int kVTIdx_SetEnable = 7;
constexpr int kVTIdx_SetShow = 9;
constexpr int kVTIdx_CreateCtrlBtn = 8; // CCtrlButton::CreateCtrl, primary +0x20

constexpr int kGridSlots = 25; // card grid, `cmp ebx,0x19` @0x00867BB4

// K7(b) -- THE DROPPER RESULT VIEW PAGES AT 20 SLOTS (4 ROWS), NOT 25.
//
// The card-rect builder (0x00863717..0x0086375B, disassembled instruction by instruction) lays the
// grid out as `ecx = (i%5)*0x21 + 8`, `eax = (i/5)*0x2d + 0x1f`, `SetRect(dst, ecx, eax, ecx+0x1b,
// eax+0x26)` for i in 0..0x18 -- so rect(i) = { 33*(i%5)+8, 45*(i/5)+31, +27, +38 } and the ROW
// BOTTOMS are 69 / 114 / 159 / 204 / 249. Those rects are canvas space on the LEFT-PAGE layer
// (kOff_CardRects = 0xB2C, drawn onto the layer created at 0x00862B84), and that layer is 256 px
// tall -- measured, not assumed: `PAINT ppm ... canvasH=256` on lines 610, 1390 and 1529 of the
// session trace the user reproduced.
//
// A % label is kPpmLabelH = 12 px. Row 4 would need 250..262 and the surface ends at 256, which is
// why round 10's label pass silently skipped the whole bottom row (`noRoom`). 6.K-K7 allows exactly
// two answers and forbids drawing the label over the card; option (a) is arithmetically impossible
// here (six pixels remain, twelve are needed), so this is option (b): the DROPPER view shows four
// rows and pages every 20 results. Slots 20..24 of that view resolve to "no card" through the same
// remap the grid already uses, so the client draws nothing in them -- and every card that IS drawn
// gets its label, which is what "the last row must look exactly like the others" means.
//
// It applies to the DROPPER view only. Mob and level searches paint no labels, so they keep all 25.
constexpr int kDropperGridSlots = 20;
constexpr int kTabCount = 9;   // colour tabs 0..8
constexpr int kTabNoCard = 9;  // the cover / initial screen; every stock control is hidden here
constexpr int kTabDropping = 2;
constexpr int kRightTabNone = 4; // not 0..3 -> Draw's sub/dec/dec/dec chain falls to its tail

constexpr int kCardOriginX = 40; // card-grid canvas, window space (0x00867B8E `add eax,0x28`)
constexpr int kCardOriginY = 25;
constexpr int kPageOriginX = 240; // right-page canvas (0x00862409 `add eax,0xf0`)
constexpr int kPageOriginY = 20;
constexpr int kLeftPageOriginX = 40; // CreateLayer(&out, 0x28, 0x19, ...) @0x00862B84
constexpr int kLeftPageOriginY = 25;

// ---- the two rows, in WINDOW coordinates --------------------------------------------------------
// Row 1 is byte-for-byte where the stock CreateCtrl put the edit and BtSearch; row 0 is one 20 px
// pitch directly above it.
constexpr int kFieldX = 49;
constexpr int kFieldW = 120;
constexpr int kFieldH = 15;
constexpr int kRowPitch = 20;
constexpr int kFieldY[2] = { 10, 30 }; // 0 = ITEM (top), 1 = MOB (bottom, stock)
constexpr int kBtnX = 175;
constexpr int kBtnY[2] = { 9, 29 };
constexpr int kRowItem = 0;
constexpr int kRowMob = 1;

// Baked box art (spec 6.F shared contract, consumed here): 122x17 Format2, origin (0,0), blitted at
// window (48,9) and (48,29). `*Box0` carries the grey placeholder, `*Box1` is the empty box.
constexpr int kBoxW = 122;
constexpr int kBoxH = 17;
constexpr int kBoxX = 48;
constexpr int kBoxY[2] = { 9, 29 };

// Search art is the "Brass Ledger" set in Custom.wz, read through the Custom/ prefix: resman's
// Custom.wz merge does not add NEW sub-properties to an existing .img, so it is not visible under
// UI/UIWindow.img (see uiDamageRank.cpp / storagebag.cpp for the same rule).
#define MB_SEARCH_ART L"Custom/UI/UIWindow.img/MonsterBookSearch/"
constexpr const wchar_t* kUOL_Box[2][2] = {
    { MB_SEARCH_ART L"itemBox0", MB_SEARCH_ART L"itemBox1" },
    { MB_SEARCH_ART L"mobBox0", MB_SEARCH_ART L"mobBox1" },
};
// Both magnifiers use the SAME node. The stock one (nId 0x7D0) gets it from StringPool[0xA1A], which
// stringpool.cpp re-points at this path; our second magnifier is skinned from it directly.
constexpr const wchar_t* kUOL_SearchButtonArt = MB_SEARCH_ART L"BtSearch";
constexpr const wchar_t* kUOL_IconBase = L"UI/UIWindow.img/IconBase/0"; // at the image ROOT
// H1.10 draws its own magnifier (the home screen hides every stock control), so it needs the plain
// canvas rather than the button's four-state node. Both spellings are tried: v83 button art is
// usually `<state>/0`, but a single-frame state is sometimes the canvas itself.
constexpr const wchar_t* kUOL_MagnifierArt[2] = {
    MB_SEARCH_ART L"BtSearch/normal/0",
    MB_SEARCH_ART L"BtSearch/normal",
};
// K2.3 -- the level magnifier is module-drawn, so it gets no hover frame from the engine. The stock
// `CCtrlButton` skin has four states under the same node and `mouseOver` is the one the engine swaps
// in while the cursor is inside a button; drawing it ourselves is the whole of "matching the stock
// buttons' feel". Same two spellings as `normal`, same per-book re-arm.
constexpr const wchar_t* kUOL_MagnifierOverArt[2] = {
    MB_SEARCH_ART L"BtSearch/mouseOver/0",
    MB_SEARCH_ART L"BtSearch/mouseOver",
};

// ---- H1.10 "search cards per level", HOME SCREEN ONLY (left tab 9) -------------------------------
// The box rides the ITEM row's window-canvas slot. That slot is PROVEN visible on the cover: round
// 5's defect report was that the item field "shows and FLICKERS on the initial screen", i.e. a blit
// at window (48,9) reaches the screen there. Everything from y >= 25 is covered by the LEFT PAGE
// layer instead (Redraw(0) runs on tab 9 too -- it reads `leftTab == 9` at its own head, 0x00863E1A
// -- and paints the cover into that layer), which is why the dropdown below is drawn on THAT canvas
// and not on the window's.
constexpr int kLvlBoxX = kBoxX;      // 48
constexpr int kLvlBoxY = kBoxY[kRowItem]; // 9
constexpr int kLvlBoxW = kBoxW;      // 122
constexpr int kLvlBoxH = kBoxH;      // 17
constexpr int kLvlArrowW = 14;       // the drop-down triangle's cell, at the box's right edge
constexpr int kLvlBtnX = kBtnX;      // 175, the stock magnifier column
constexpr int kLvlBtnY = kBtnY[kRowItem];
constexpr int kLvlBtnW = 34;         // BtSearch art is 34x17 (spec 6.E)
constexpr int kLvlBtnH = 17;
constexpr int kLvlRowH = 14;         // one dropdown row
constexpr int kLvlMinLevel = 0;
constexpr int kLvlMaxLevel = 5;
constexpr int kLvlCount = kLvlMaxLevel - kLvlMinLevel + 1;
// -------------------------------------------------------------------------------------------------
// K2.1 -- THE LIST IS FLUSH UNDER THE FIELD, AND THAT COSTS ONE PARK.
//
// Round 8 started the list at y 46 to dodge the stock edit's rect (49,30)-(169,45), because it could
// not settle whether a HIDDEN control still swallows a click. Round 10 settles it, twice over:
//
//   * `CWnd::HitTest` (0x009E01E7), read instruction by instruction: it walks the child list at
//     `[this+0x60]`, translates the point by each child's own vector (`get_rx` 0x00425AEF /
//     `get_ry` 0x00425B16 at 0x009E0254..0x009E0262) and calls `[childvt+0x18]` == `CCtrlWnd::IsHit`
//     (0x004DFECE), which is `PtInRect({0,0,w,h}, localPt)` and nothing else. There is no read of
//     ctrl+0x30 (the shown flag) anywhere in either function -- so YES, a hidden control still owns
//     its rect.
//   * and the shipped log agrees from the other side: every `CLICKRAW ... field=1` line in the whole
//     session carries `editRow=0`, i.e. the book only ever RECEIVES a click in the mob-row band when
//     the edit is physically on the other row. Not one click inside the edit's own rect reached
//     `OnMouseButton`.
//
// So the list may start at `kLvlBoxY + kLvlBoxH` -- flush, no gap -- only while the edit has been
// PARKED off-screen, exactly the way BtSearch2 already is on this screen (ParkStockEdit). If the
// park cannot be verified by read-back, `LevelListY()` falls back to the old safe y, so a failed
// park costs a 20 px gap and never a dead dropdown row.
constexpr int kLvlListX = kLvlBoxX;
constexpr int kLvlListYFlush = kLvlBoxY + kLvlBoxH; // 26 -- touching the box, no gap
constexpr int kLvlListYSafe = 46;                   // clears the edit's rect (49,30)-(169,45)
constexpr int kLvlListW = kLvlBoxW;
constexpr int kLvlListH = kLvlRowH * kLvlCount;
constexpr unsigned kLvlFillColor = 0xFFFFFFFF;   // opaque white, like the baked boxes
constexpr unsigned kLvlBorderColor = 0xFF8C8C8C; // 1 px grey frame, like the stock edit
constexpr unsigned kLvlHintColor = 0xFF9A9A9A;   // the grey placeholder tone
constexpr unsigned kLvlTextColor = 0xFF000000;
constexpr unsigned kLvlSelColor = 0xFFD8E4F8;    // the selected row's highlight

// -------------------------------------------------------------------------------------------------
// K1 -- the droppers-from-item view keeps its COUNT DIGIT. (Reverses I1.6, at the user's request.)
//
// I1.6 had this view sample the parchment next to each card and paint a 22x12 patch over the card's
// bottom-left corner, where `Draw`'s counter block inserts `UI/Basic.img/ItemNo`
// (0x0086501E..0x00865045: `y = rect.bottom - digitHeight - 1`, `x = rect.left + 1`). Round 10
// reverses that: the digit is drawn by the client exactly as it is on every other grid -- 0..5, or
// the medal -- and this module only adds the per-mob drop % in a label BELOW the card. Nothing of
// the card art is overpainted any more, so there is no sample, no patch and no colour to guess; the
// `SampleParchment` helper and the two patch constants are gone with it.
//
// The label still needs room below the bottom row, so the canvas height is read once per pass and a
// label that would fall off the end of the layer is simply not drawn -- never moved back on top of
// the digit, which is the thing this item exists to stop.
constexpr unsigned kPpmLabelBg = 0xFFFFFFFF; // white box   -- monsterBookDrops.cpp's kLabelBg
constexpr unsigned kPpmLabelFg = 0xFF000000; // black text  -- ...kLabelFg
constexpr int kPpmLabelPadX = 2;             // ...kLabelPadX
constexpr int kPpmLabelH = 12;

// Control ids. 0x7D6 / 0x7D7 are the two CCtrlTabs and OnChildNotify special-cases them, so neither
// may be reused; 0x7D8..0x7DB fall out of OnButtonClicked's `sub eax,0x7D0` + four `dec`.
constexpr unsigned kNId_BtSearch = 0x7D0;
constexpr unsigned kNId_CardPrev = 0x7D1;
constexpr unsigned kNId_CardNext = 0x7D2;
constexpr unsigned kNId_ListPrev = 0x7D3;
constexpr unsigned kNId_ListNext = 0x7D4;
constexpr unsigned kNId_StockEdit = 0x7D5;
constexpr unsigned kNId_BtSearch2 = 0x7DA;

// ---- protocol (lockstep with Cosmic MonsterBookQueryHandler / PacketCreator) --------------------
constexpr uint16_t kOp_C2S_MonsterBookQuery = 0x3732; // Server RecvOpcode.MONSTER_BOOK_QUERY (reference used 0x372B)
constexpr uint8_t kQuery_ItemName = 1; // C2S [1][str q]    -> S2C [1][str q][short n][n * i32 item]
// I2, NEW WIRE FORMAT this round: type 2's rows are PAIRS now.
//   C2S [2][int item] -> S2C [2][i32 item][short n][ n * { i32 mobId, i32 ppm } ]
// `ppm` is that mob's effective chance for THAT item at the asking player's live rates, i.e. the
// same units monsterBookDrops.cpp already renders (percent = ppm / 10000.0). The old 4-byte-per-row
// shape is still accepted -- see MonsterBookSearch_OnPacket -- so a client built against this round
// does not go blind against a server that has not shipped I2 yet.
constexpr uint8_t kQuery_Droppers = 2;
constexpr size_t kWireRow_ItemsV1 = 4;    // type 1: i32 itemId
constexpr size_t kWireRow_DroppersV2 = 8; // type 2 NEW: i32 mobId + i32 ppm
constexpr size_t kWireRow_DroppersV1 = 4; // type 2 OLD: i32 mobId
constexpr int kMinQueryLen = 3;        // MonsterBookQueryHandler.MIN_QUERY_LENGTH
constexpr int kMaxQueryLen = 40;
constexpr int kMaxWireRows = 400;

constexpr int kMaxCardResults = 500; // 20 pages of 25, or 25 of 20 in the K7(b) dropper view
constexpr int kItemSlots = 16;       // 4x4 -- must match monsterBookDrops.cpp's regridded page
constexpr DWORD kSendThrottleMs = 250;
constexpr DWORD kAwaitTimeoutMs = 6000;
// (J-D: the old `kBookGoneMs` idle timeout is DELETED. OnCreate, OnDestroy and the book-pointer
// change are the only resets; see TickBody.)
constexpr DWORD kCtrlRetryMs = 400;     // spacing between second-magnifier creation attempts
constexpr int kCtrlMaxAttempts = 4;     // ... per BOOK, re-armed by OnCreate (never per session)
constexpr int kTabParkOffset = 3000;    // how far off-screen the content tab strip is parked

// -------------------------------------------------------------------------------------------------
// 6.G-G0.5 vs 6.K-K4 -- WHO OWNS THE CONTENT TAB STRIP WHILE THE ITEM VIEW IS UP
//
// These two spec items are in direct conflict and only the spec's author can settle it, so the
// conflict is a named constant rather than a silent choice:
//
//   * 6.G-G0.5 (round 6, user point 6) asked for the four content tabs to be HIDDEN during the item
//     view, "by MOVING the CCtrlTab far off-screen ... position is engine-native and cannot be
//     ignored the way SetShow evidently was", and accepted "all-four-hidden" as an outcome.
//     Shipped, and the log proves it takes: `TAB park home=(439,25)` / `move -> (3439,25) ok=1`
//     (session trace, repeated across the run ...).
//   * 6.K-K4 (round 10) asks for the strip's stored selection to be 2 "so the strip highlights
//     Dropping instead of Basic Info" -- which presumes the strip is ON SCREEN during that view.
//
// A control parked at x = 3439 with SetShow(0) highlights nothing, so K4's WRITE is correct and
// K4's stated PURPOSE is unreachable at the same time. This flag is the whole of the difference:
// `true` keeps round 6's behaviour (strip hidden, K4's selection stored but invisible), `false`
// leaves the strip in place during the item view so K4 becomes visible -- and makes its four tabs
// clickable again, which WatchNavigation already handles by tearing the view down on a right-tab
// change. The HOME-screen park (I1.4, in J-E's must-not-regress list) is unaffected either way.
//
// Default: `true`, i.e. no behaviour change in this round. The K4 write below now logs the park
// state next to it, so the next session's log settles the question with evidence instead of memory.
constexpr bool kParkTabStripForItemView = true;

// The content tab strip's STOCK home, straight out of its own CreateCtrl push sequence at
// 0x00862644: `mov [ebp-0x34],0x1B7 / [ebp-0x30],0x19 / [ebp-0x2C],0x1FA / [ebp-0x28],0xDC`
// -> rect {439, 25, 506, 220} for nId 0x7D7. Used ONLY as a recovery value: if the position we
// read back is already parked (a previous book's park that our bookkeeping lost, e.g. because a
// stall tripped the book-gone reset while the strip was off-screen), saving it as "home" would
// double the offset every item view and the four tabs would never come back for that book. So an
// out-of-range read is refused and the strip is put back HERE instead. A book is 475 px wide, so
// anything past kTabSaneMaxX cannot be a real position inside it. (The "stall" case is gone with
// J-D's idle timeout; `adopt-new-book` remains.)
constexpr int kTabHomeX = 439;
constexpr int kTabHomeY = 25;
constexpr int kTabSaneMaxX = 1000;

// monsterBookDrops.cpp rewrites the Dropping page from 4x5 to 4x4 by patching the list builder's
// `cmp dword ptr [ebp-0x1C], 0x14` at 0x008674CF (imm8 at +3) from 20 to 16. `slot -> reward
// ordinal` below is only valid once that landed; on a client where the signature did not match,
// the client still pages 20 and a single click would open the droppers of the WRONG item. Read
// the byte instead of assuming -- it is the same fact, read from the same place, and read-only.
constexpr uintptr_t kAddr_DropPagingImm = 0x008674D2;

// =================================================================================================
// originals
// =================================================================================================
using UpdateFn = void(__fastcall*)(void*, void*);
using MouseButtonFn = void(__fastcall*)(void*, void*, unsigned int, unsigned int, int, int);
using MouseMoveFn = int(__fastcall*)(void*, void*, int, int);
using OnCreateFn = void(__fastcall*)(void*, void*, void*);
using OnDestroyFn = void(__fastcall*)(void*, void*);
using WndDrawFn = void(__fastcall*)(void*, void*, void*);
using RedrawPane0Fn = void(__fastcall*)(void*, void*);
using ButtonClickedFn = void(__fastcall*)(void*, void*, unsigned int);
using GetSlotCardFn = void*(__fastcall*)(void*, void*, void*, int, int, int);
using SendPacketFn = void(__thiscall*)(void*, COutPacket*);

auto CUIMonsterBook_Update_Orig = reinterpret_cast<UpdateFn>(kAddr_Update);
auto CUIMonsterBook_OnMouseButton_Orig = reinterpret_cast<MouseButtonFn>(kAddr_OnMouseButton);
auto CUIMonsterBook_OnMouseMove_Orig = reinterpret_cast<MouseMoveFn>(kAddr_OnMouseMove);
auto CUIMonsterBook_OnCreate_Orig = reinterpret_cast<OnCreateFn>(kAddr_OnCreate);
auto CUIMonsterBook_OnDestroy_Orig = reinterpret_cast<OnDestroyFn>(kAddr_OnDestroy);
auto CUIMonsterBook_WndDraw_Orig = reinterpret_cast<WndDrawFn>(kAddr_WndDrawThunk);
auto CUIMonsterBook_RedrawPane0 = reinterpret_cast<RedrawPane0Fn>(kAddr_RedrawPane0);
auto CUIMonsterBook_OnButtonClicked = reinterpret_cast<ButtonClickedFn>(kAddr_OnButtonClicked);
auto CUIMonsterBook_GetSlotCard = reinterpret_cast<GetSlotCardFn>(kAddr_GetSlotCard);
auto CClientSocket_SendPacket = reinterpret_cast<SendPacketFn>(kAddr_CClientSocket_SendPacket);

// =================================================================================================
// tiny SEH helpers -- MSVC forbids __try in a function that also needs C++ unwinding, and most
// callers below hold vectors / smart pointers, so every raw memory touch funnels through these.
// =================================================================================================
int SafeReadInt(const void* p, uint32_t off) {
    if (!p) {
        return 0;
    }
    __try {
        return *reinterpret_cast<const int*>(reinterpret_cast<const char*>(p) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void* SafeReadPtr(const void* p, uint32_t off) {
    if (!p) {
        return nullptr;
    }
    __try {
        return *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(p) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool SafeWriteInt(void* p, uint32_t off, int value) {
    if (!p) {
        return false;
    }
    __try {
        *reinterpret_cast<int*>(reinterpret_cast<char*>(p) + off) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A ZArray<T> stores its element count in the dword before the data -- the same `[eax-4]`
// GetSlotCard bounds-checks against at 0x00867A81 / 0x00867A96.
int SafeArrayCount(const void* pArray) {
    if (!pArray) {
        return 0;
    }
    __try {
        return *(reinterpret_cast<const int*>(pArray) - 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void* SafeIndexPtr(const void* pArray, int i) {
    if (!pArray) {
        return nullptr;
    }
    __try {
        return reinterpret_cast<void* const*>(pArray)[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Copy a ZXString<char>'s characters out under SEH. The handle at ctrl+0x34 points straight at the
// characters (ZXString<T>::_m_pStr), which is exactly what CCtrlEdit::GetText assigns from.
int SafeReadCString(const void* pOwner, uint32_t off, char* out, int cap) {
    if (!out || cap <= 0) {
        return 0;
    }
    out[0] = '\0';
    if (!pOwner) {
        return 0;
    }
    __try {
        const char* s = *reinterpret_cast<const char* const*>(
                reinterpret_cast<const char*>(pOwner) + off);
        if (!s) {
            return 0;
        }
        int n = 0;
        while (n < cap - 1 && s[n] != '\0') {
            out[n] = s[n];
            ++n;
        }
        out[n] = '\0';
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return 0;
    }
}

bool InGame() {
    __try {
        void* user = *reinterpret_cast<void**>(kAddr_CUserLocal_Instance);
        if (!user) {
            return false;
        }
        return *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(user) + kOff_CUserLocal_CharId)
                != 0;
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

// =================================================================================================
// engine wrappers
// =================================================================================================

// Call one of the four state slots on a control's IUIMsgHandler sub-object (ctrl+4) -- exactly how
// RefreshCtrls shows and hides the stock pair (`lea ecx,[eax+4]` then `call [eax+0x24]`).
void CallCtrlSetter(void* pCtrl, int slot, int value) {
    if (!pCtrl) {
        return;
    }
    __try {
        void* sub = reinterpret_cast<char*>(pCtrl) + 4;
        void** vt = *reinterpret_cast<void***>(sub);
        if (!vt || !vt[slot]) {
            return;
        }
        reinterpret_cast<void(__thiscall*)(void*, int)>(vt[slot])(sub, value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Rate-limited (observe-then-correct re-tries every tick, so an un-limited log would flood
        // and blow the file cap), but never silent: a faulting SetShow/SetEnable is exactly how a
        // control quietly stops obeying us.
    }
}

// OBSERVE-THEN-CORRECT: read the control's REAL flag and write only when it disagrees, so a stale
// cached latch can never flip a control and re-asserting every tick costs one read.
bool ShowCtrl(void* pCtrl, int bShow) {
    if (!pCtrl) {
        return false;
    }
    const int want = bShow ? 1 : 0;
    if ((SafeReadInt(pCtrl, kOff_Ctrl_Shown) ? 1 : 0) == want) {
        return false;
    }
    CallCtrlSetter(pCtrl, kVTIdx_SetShow, want);
    return true;
}

bool EnableCtrl(void* pCtrl, int bEnable) {
    if (!pCtrl) {
        return false;
    }
    const int want = bEnable ? 1 : 0;
    if ((SafeReadInt(pCtrl, kOff_Ctrl_Enabled) ? 1 : 0) == want) {
        return false;
    }
    CallCtrlSetter(pCtrl, kVTIdx_SetEnable, want);
    return true;
}

// The control's own position vector. Every control that lives in a CWnd's child list has one, or
// CWnd::HitTest would throw E_POINTER on it (0x009E022D) -- so a null here means "not a control".
IWzVector2D* CtrlVector(void* pCtrl) {
    return reinterpret_cast<IWzVector2D*>(SafeReadPtr(pCtrl, kOff_Ctrl_Vector));
}

bool CtrlReadPos(void* pCtrl, int* outX, int* outY) {
    IWzVector2D* pVec = CtrlVector(pCtrl);
    if (!pVec) {
        return false;
    }
    int x = 0, y = 0;
    try {
        if (FAILED(pVec->get_rx(&x)) || FAILED(pVec->get_ry(&y))) {
            return false;
        }
    } catch (...) {
        return false;
    }
    if (outX) {
        *outX = x;
    }
    if (outY) {
        *outY = y;
    }
    return true;
}

// The engine's own placement call -- `[vec_vtbl+0x90](x, y, vtEmpty, vtEmpty)`, byte-for-byte what
// CCtrlWnd::CreateCtrl finishes with at 0x004DFD40.
bool CtrlMoveTo(void* pCtrl, int x, int y) {
    IWzVector2D* pVec = CtrlVector(pCtrl);
    if (!pVec) {
        return false;
    }
    try {
        return SUCCEEDED(pVec->raw_RelMove(x, y, vtEmpty, vtEmpty));
    } catch (...) {
        return false;
    }
}

void* FocusOwner() {
    __try {
        void* pMgr = *reinterpret_cast<void**>(kAddr_CWndMan_Instance);
        if (!pMgr) {
            return nullptr;
        }
        return *reinterpret_cast<void**>(reinterpret_cast<char*>(pMgr) + kOff_WndMan_Focus);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// CWndMan::SetFocus takes the ctrl+4 sub-object, refuses a control whose parent is not the active
// window (0x009E32BC), refuses a disabled one (0x009E32D5) and returns immediately when it is
// already the focus (0x009E32DE) -- idempotent, and the fast path is checked here too.
bool FocusCtrl(void* pCtrl) {
    if (!pCtrl) {
        return false;
    }
    __try {
        void* pMgr = *reinterpret_cast<void**>(kAddr_CWndMan_Instance);
        if (!pMgr) {
            return false;
        }
        void* sub = reinterpret_cast<char*>(pCtrl) + 4;
        void** ppFocus = reinterpret_cast<void**>(reinterpret_cast<char*>(pMgr) + kOff_WndMan_Focus);
        if (*ppFocus == sub) {
            return true;
        }
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kAddr_CWndMan_SetFocus)(pMgr, sub);
        return *ppFocus == sub;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// I1.5 -- hand the focus back to the WINDOW, byte-for-byte what CUIMonsterBook::OnMouseButton's own
// head does for any click that misses the edit: `[book+0x64] = 0` then
// `CWndMan::SetFocus(book+4)` (0x008621B9..0x008621CC). Used when this module consumes a click that
// is NOT a field row, so a focused edit can never be left holding the keyboard -- or, if the engine
// routes anything by focus, holding a click that belonged to another control.
void FocusWindowSelf(void* pBook) {
    if (!pBook) {
        return;
    }
    __try {
        void* pMgr = *reinterpret_cast<void**>(kAddr_CWndMan_Instance);
        if (!pMgr) {
            return;
        }
        *reinterpret_cast<int*>(reinterpret_cast<char*>(pBook) + 0x64) = 0;
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kAddr_CWndMan_SetFocus)(
                pMgr, reinterpret_cast<char*>(pBook) + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// `CCtrlTab::SetSelected` (0x008603DF) in its own frame, because it NOTIFIES the parent
// synchronously (0x008606AF: `OnChildNotify(nId, 0x1F4, index)`) and therefore runs a good deal of
// client code -- and because MSVC forbids `__try` in any function that also needs C++ unwinding, so
// the callers (which hold RAII objects and vectors) cannot host the guard themselves.
void CallTabSetSelected(void* pTab, int index, const char* why) {
    if (!pTab) {
        return;
    }
    __try {
        reinterpret_cast<void(__thiscall*)(void*, int)>(kAddr_CCtrlTab_SetSelected)(pTab, index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// K8 -- `CUIMonsterBook::SelectCurrentSlot()`, in its own frame because __try cannot share a
// function with C++ object unwinding (the same reason SafeGetCardLevel exists). It re-enters this
// module through GetSelectedCard -> GetSlotCard -> our remap, and can re-enter CCtrlTab::SetSelected
// through SetSelectedCard, so it is called on a settled state and never from inside a paint pass.
bool CallSelectCurrentSlot(void* pBook) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(kAddr_Book_SelectCurrentSlot)(pBook);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RefreshCtrlsNow(void* pBook) {
    if (!pBook) {
        return;
    }
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(kAddr_RefreshCtrls)(pBook);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// CCtrlWnd::CreateCtrl stamps the id at ctrl+0x14 and the parent at ctrl+0x24, so those two
// identify a control exactly. A closed book frees its children and a fresh CUIMonsterBook can land
// on the same heap address, so nothing is ever poked without this check first.
bool IsOurCtrl(void* pCtrl, void* pBook, unsigned nId) {
    if (!pCtrl || !pBook) {
        return false;
    }
    return SafeReadInt(pCtrl, kOff_Ctrl_Id) == static_cast<int>(nId)
            && SafeReadPtr(pCtrl, kOff_Ctrl_Parent) == pBook;
}

// =================================================================================================
// per-book state.  EVERY field here dies with the book. There is NO session-scoped disable.
// =================================================================================================
struct SearchState {
    void* book = nullptr;
    DWORD seenTick = 0;

    // The one text input: the STOCK CCtrlEdit (book+0xAE8).
    //
    // H1.1: `editRow` is NOT a wish -- it is a READBACK. It is only ever assigned from the
    // position the engine reports for ctrl+0x18 AFTER a raw_RelMove, because that same vector is
    // what the caret pass reads (`get_rx` at 0x004CAFDD, `get_ry` at 0x004CB729) and what
    // CWnd::HitTest routes clicks by (0x009E01E7). A flag that can disagree with those two is the
    // exact defect round 7 reported: caret painted on the mob row, keystrokes filed under item.
    int editRow = kRowMob;                 // where the client created it -- and where it still IS
    int wantRow = -1;                      // a move that has NOT taken yet; the tick re-tries it
    int wantRowTries = 0;
    DWORD wantRowNextTry = 0;
    std::string rowText[2];                // the row that is NOT hosting keeps its text here
    // I1.1: what the MODULE last painted as that row's TEXT. Under the ownership rule a HOSTING row
    // is always painted with an EMPTY string (the edit owns its glyphs), so this only ever tracks
    // the stashes -- which is exactly what makes the round-7 per-tick dirty storm impossible.
    std::string rowPainted[2];
    int paintedHostRow = -2;               // the hosting row the last paint pass drew for
    // ...and the value the tick has ALREADY raised a pane-0 dirty flag for. Without this pair the
    // detector re-dirties every single tick whenever a paint pass is skipped (no canvas, wrong
    // screen) while a row holds text -- an unbounded per-frame Redraw(0), each of which builds a
    // fresh `Canvas` object at 0x00863E2E. See the I1.5 note.
    std::string rowDirtiedFor[2];
    bool rowDirtiedValid[2] = { false, false };
    // K3.1 -- and the box VARIANT, tracked separately: it follows the row's LIVE text now, which for
    // a window-surface hosting row is not what `rowDirtiedFor` records.
    int rowVariantDirtiedFor[2] = { -1, -1 };
    int hostDirtiedFor = -2;
    bool editFillRestored = false;         // the WZ box was missing -> the opaque fill went back
    // K2.1 -- the stock edit is off-screen because the home screen's dropdown needs its rect. Only
    // ever set from a READ-BACK, because `LevelListY()` keys the dropdown's geometry on it and a
    // park that only happened in our bookkeeping would put two rows under an invisible control.
    bool editParked = false;

    // Our second magnifier (nId 0x7DA). Owned by the book once created.
    // I1.2: it is the ITEM row's magnifier and NOTHING else. Off a tier screen it is parked
    // off-screen, because a merely hidden control still answers CWnd::HitTest.
    void* btSearch2 = nullptr;
    int ctrlAttempts = 0;
    DWORD ctrlNextTry = 0;
    bool mag2Parked = false;

    // Card index of the OPEN book.
    bool cardsBuilt = false;

    // LEFT page result view (mob-name hits, or the droppers of an item).
    bool cardView = false;
    std::vector<int> cardResults; // indices into g_cards
    int cardPage = 0;
    // I1.6 -- this card view came from a type-2 droppers reply, so each card's count digit is
    // replaced by that mob's drop % for `dropperItem`. Only this view; never the mob-name or
    // level-filter views.
    bool cardFromDroppers = false;
    int dropperItem = 0;

    // RIGHT page result view (item-name hits).
    bool itemView = false;        // "we have results"
    bool itemViewLive = false;    // "...and they own the page right now" (the Update-side flag)
    std::vector<int> itemResults; // item ids
    int itemPage = 0;
    std::string itemQuery;

    // Borrowed chrome, all restored the moment the item view goes away.
    // I1.4: the content tab strip is parked for TWO reasons now -- the item view owning the right
    // page, and the HOME screen (where the four tabs stayed clickable even though the drops module
    // forces the index back to 0). One park, one restore, one reason string in the log.
    bool tabParked = false;
    int tabSaveX = 0;
    int tabSaveY = 0;
    const char* tabParkReason = "";
    bool arrowsForced = false;

    // Navigation watcher (G0.4).
    int watchLeft = -2;
    int watchRight = -2;

    // (The old `boxVariant[2]` cache is GONE. Under I1.1 the variant is a pure function of
    // `editRow` + the stash -- `BoxVariantForRow` -- and both of those already drive the tick's
    // dirty detector, so a second copy could only ever disagree with them.)

    // H1.10 -- the home screen's "search cards per level" control.
    bool levelOpen = false; // the dropdown is unrolled
    int levelSel = -1;      // 0..5, -1 = nothing chosen yet
    bool levelView = false; // the current card view came from the level search
    // K2.3 -- the cursor is inside the module-drawn magnifier's rect right now.
    bool levelMagHover = false;
    // K2.2 -- what the last level-box PAINT actually put on the window canvas. The window canvas has
    // no dirty flag of its own (only `CWnd::Draw` touches it, and only on transitions), which is why
    // the shipped log carries `PAINT level box` at CREATE and at screen changes and NEVER after a
    // `LEVEL pick`. `EnsureLevelBoxFresh` compares these three and repaints on the spot.
    int lvlPaintedSel = -2;
    int lvlPaintedOpen = -1;
    int lvlPaintedHover = -1;
    // ...and round 11's ATTEMPT guard for the same trio. `PaintLevelControl` records the painted
    // trio only after it is past its own three early exits, so a bail used to leave the freshness
    // check permanently unsatisfied -- a repaint plus a MarkAllDirty on every Update AND every tick,
    // i.e. an unbounded per-frame Redraw(0). One attempt per state change, no more.
    int lvlAttemptedSel = -2;
    int lvlAttemptedOpen = -1;
    int lvlAttemptedHover = -1;
    // Round 11: a window-strip repaint was REFUSED out-of-band (the edit owns those glyphs) and the
    // CWnd::Draw hook still owes it. Cleared by the first window-surface paint that actually lands.
    bool stripRepaintPending = false;
    // Round 11: the last OnMouseMove this module answered for THIS book. The engine sends no
    // "cursor left the window" message to a UI window, so a hover state can only be retired by
    // noticing that the moves have stopped.
    DWORD lastMoveTick = 0;
    // Round 11: the blink phase the left-page caret was last drawn for, so the tick raises pane 0
    // exactly twice a second while a module-drawn caret is alive, and not at all otherwise.
    int caretPhaseDirtiedFor = -1;

    // K5.2 -- THE CARD PICK IN FLIGHT.
    //
    // Armed for the duration of ONE `OnMouseButton` call on a result card, because the client's own
    // pick path re-enters the grid remap with coordinates that mean something else while a result
    // view owns the grid. Chain, all of it byte-verified:
    //   0x00862360 hit test  -> 0x00867BDC GetSlotCard(tab, +0x5B0, hitSlot)      = the right card
    //   0x00862371 GetSelectedCard(0x00867A2F) -> GetSlotCard(tab, +0x5B0, +0x5B8)
    //   0x008623A8 SetSelectedCard(0x0086793F) when the two differ:
    //       0x00867971 CCtrlTab::SetSelected(record+8) -- and that function writes the new index at
    //                  0x00860674 and then NOTIFIES THE PARENT at 0x008606AF
    //                  (`OnChildNotify(nId, 0x1F4, index)`), synchronously;
    //       0x00861FD0 CUIMonsterBook::OnChildNotify(0x7D6, 0x1F4): SetCardPage(0), SetSelSlot(0),
    //                  SetListPage(0), then 0x00867923 `SelectCurrentSlot`, which is
    //                  GetSelectedCard + SetSelectedCard -- i.e. it asks the grid for slot (0,0)
    //                  AFTER zeroing both indices, and under our remap that is `cardResults[0]`,
    //                  THE FIRST CARD OF THE RESULT GRID. It then selects it for real: art, HP/MP,
    //                  Dropping list, Found In list.
    // The shipped log shows it happening: `DROPS SETMOBINFO mob=3300008` (== cardResults[0]) 21 ms
    // before `DROPS SETMOBINFO mob=4220000` / `CLICK cardresult slot=2 page=0 -> mob=4220000`, on the
    // first click into a fresh droppers view, and never on a click that did not change the colour
    // tier (the notify only fires when `CCtrlTab::SetSelected` actually changes the index --
    // 0x00860441 `cmp esi, eax / je`).
    //
    // The fix is to make that ONE query answer with the card the player clicked. `pickSlot`/
    // `pickPage` are the click's own coordinates and `pickCardIndex` the card they resolve to; the
    // remap answers a `(page 0, index 0)` request with it while armed. The other two queries cannot
    // collide: the hit test asks `(pickPage, pickSlot)` (identical to the override when that is
    // (0,0)), and the comparison asks `(page, +0x5B8)` with `+0x5B8` deliberately set to -1 -- which
    // the remap refuses, so `GetSelectedCard` answers "no card", the `setne` at 0x00862379 is always
    // true and the click can never be swallowed for being on the already-selected card either.
    bool pickArmed = false;
    int pickCardIndex = -1;
    int pickPage = 0;
    int pickSlot = -1;

    // H1.9 -- the card tooltip currently on screen (0 = none). Kept so an unchanged hover does not
    // rebuild the tooltip 60 times a second.
    int hoverCardMob = 0;
    bool tipOurs = false; // we own the book's CUIToolTip right now and must clear it on the way out
    DWORD lastHoverTick = 0; // last OnMouseMove we answered; the tick expires a stranded tooltip

    // Wire.
    bool pendingArmed = false;
    uint8_t pendingType = 0;
    std::string pendingText;
    int pendingItem = 0;
    DWORD lastSendTick = 0;
    bool awaitItems = false;
    bool awaitDroppers = false;
    DWORD awaitSince = 0;
    bool replyReady = false;
    int replyType = 0;
    std::vector<int> replyIds;
    std::vector<int> replyPpm;  // I2 -- parallel to replyIds for type 2; empty on the old format
    int replyItem = 0;          // I2 -- the item the type-2 reply is about

    // Deferred (flush-only) work.
    bool wantMobSearch = false;
    std::string mobQuery;
    bool wantLevelSearch = false; // H1.10 -- the index build + ~860 GetCardLevel calls

    // I1.10 -- ENTER runs the hosting row's search. Edge-detected on the tick, because no clean key
    // route into this window exists: CUIMonsterBook has no OnKey of its own, the edit consumes its
    // own keystrokes, and OnChildNotify only forwards param1 == 100.
    bool enterWasDown = false;

    // ---- J-B round 10 -- PRESS IDENTITY for the top band ----------------------------------------
    //
    // Round 9 inferred the drag band from CLICKRAW lines. Round 10 has it from the bytes, and the
    // bytes say more than the log could. The book's own HitTest override is primary[9] = 0x0092C5E4:
    //
    //     r = CWnd::HitTest(x, y, &pChild);          // 0x009E01E7 -- 0 outside, 2 inside
    //     if (r == 0)   return 0;
    //     if (pChild)   return r;                    // a control owns the point -> 2
    //     return (y >= 0 && y < 0x19) ? 1 : 2;       // 0x0092C604..0x0092C616 -- y 0..24 = CAPTION
    //
    // and the UI mouse dispatcher (0x009E3B10..0x009E3FD2) treats those three answers as:
    //
    //     0x201 with HitTest == 1 -> [CWndMan+0x84] = this window (0x009E3F76), then RETURN.
    //                                OnMouseButton is NEVER reached for that DOWN.
    //     0x203, 0x202            -> no HitTest at all (0x009E3F18/0x009E3F7E fall through), straight
    //                                to [msgvt+8] == OnMouseButton at 0x009E3FBF.
    //
    // So the ITEM row, the level box and the level magnifier -- all of them inside window y 9..25 --
    // receive ONLY 0x203 and 0x202, exactly as the round-9 session logged, and every action up there
    // must run on the 0x202 UP. That turns the whole problem into one question: "is this UP the end
    // of a press that began HERE?". These fields answer it.
    //
    //   pressConsumed  set ONLY by a 0x203, the second click of a single double-click gesture, so the
    //                  UP that follows it cannot repeat a non-idempotent action. Round 9 had no such
    //                  guard in that direction and a double-click on the level box opened the list on
    //                  the first UP and shut it again on the second. Cleared by the UP that ends the
    //                  press and by any fresh 0x201, and it self-expires after the double-click time
    //                  so it can NEVER strand the band -- which is what round 9's single latch did:
    //                  it was set by the MOB row's DOWN and cleared only by a later DOWN, and since
    //                  the band delivers no DOWN at all, one mob-field click killed the item field
    //                  and the entire home-screen level control for the rest of the session.
    //   pressDownSeen  a 0x201 belonging to the CURRENT press reached this handler, i.e. the press
    //                  began somewhere the drag test let through (outside the caption band, or on a
    //                  control). Its point is the press ORIGIN: an UP that resolves to a different
    //                  target is a drag-release, not a click, and is refused.
    //   pressDownTick  when that origin was recorded -- and the repair for round 10's own version of
    //                  the stuck-latch bug. `pressDownSeen` was cleared in exactly ONE place, the
    //                  WM_LBUTTONUP branch of this hook, and a DOWN's UP does not have to come back
    //                  here: the dispatcher recomputes the target window from the CURSOR at
    //                  0x009E42B2 for every message that is not a drag-captured DOWN, and drops the
    //                  message entirely (0x009E3E8C) when the child under the release point is
    //                  disabled (`[childvt+0x20]` @0x009E43C5) or hidden (`[childvt+0x28]`
    //                  @0x009E43CC). The shipped log caught it: line 345 `CLICKRAW msg=0x201 at
    //                  (472,61) ... downSeen=1`, no 0x202 for 3.7 s (CLICKRAW is deliberately not
    //                  rate-limited, so the UP provably never arrived), then line 368 refusing a
    //                  fresh, deliberate click on the level box as "moved-off-origin" against that
    //                  3.7-second-old point -- the two-clicks-to-work symptom J-B exists to remove.
    //                  So the origin now expires on age, on the button reading physically UP, and on
    //                  a new DRAG begin, exactly like `pressConsumed` does.
    //   dragTick       the last frame on which [CWndMan+0x84] pointed at THIS book, sampled by the
    //                  Update pump. A window drag zeroes that pointer (0x009E3CA5) microseconds
    //                  before the same call dispatches the UP that ended it, so "it was still set a
    //                  frame ago" is how the release of a window drag is told from a click.
    bool pressConsumed = false;
    DWORD pressConsumedTick = 0;
    bool pressDownSeen = false;
    DWORD pressDownTick = 0;
    int pressDownX = 0;
    int pressDownY = 0;
    //   dragMoved      and the half that matters: EVERY press in the band starts a drag, a click
    //                  being a drag of zero distance, so the flag alone would refuse every click up
    //                  there. What separates them is whether the window actually travelled while the
    //                  UI manager held it -- see WatchWindowDrag.
    DWORD dragTick = 0;
    bool dragActive = false;  // edge state, so a drag is logged once at each end and not per frame
    bool dragMoved = false;   // the window travelled more than kDragSlopPx during the current drag
    bool dragAnchorOk = false;
    int dragAnchorX = 0;
    int dragAnchorY = 0;

    // J-C -- which screen the (48,9) strip was last PAINTED for: -2 never, -1 unknown tab,
    // 0 tier, 1 home. The strip lives on the window canvas, which is a retained buffer, so a
    // home<->tier transition has to repaint it in BOTH directions or the previous screen's box
    // stays on screen (the round-9 "white box with no placeholder" on tier screens).
    int stripScreen = -2;
};

SearchState g_st;

// Bumped by OnCreate (and by every other reset path). Everything that used to be a once-only
// session latch now remembers WHICH generation it last tried in, so a failed probe automatically
// re-arms on the next book -- spec 6.G G0.2.
unsigned g_bookGen = 1;

// One card record of the open book, addressed the way GetSlotCard addresses it.
struct CardLoc {
    int tab;
    int page;
    int index;
    int cardId;
    int mobId;
};
std::vector<CardLoc> g_cards;     // every card in the book, in book order
std::vector<std::string> g_lower; // parallel lower-cased mob names
void* g_cardsForBook = nullptr;

void ResetForClosedBook(const char* reason);

// =================================================================================================
// small helpers
// =================================================================================================
std::string ToLower(const std::string& s) {
    std::string o = s;
    for (char& c : o) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return o;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= ' ') {
        ++a;
    }
    while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') {
        --b;
    }
    return s.substr(a, b - a);
}

constexpr int kTabUnknown = -1;

// A left-tab read that cannot lie: an unreadable control answers "unknown", never "9". Answering 9
// (the cover, where everything is hidden) on a bad frame is what made the item row flicker.
int LeftTabOrUnknown(void* pBook) {
    void* pTab = SafeReadPtr(pBook, kOff_LeftTabCtrl);
    if (!pTab) {
        return kTabUnknown;
    }
    const int t = SafeReadInt(pTab, kOff_TabSelected);
    return (t >= 0 && t <= kTabNoCard) ? t : kTabUnknown;
}

int RightTabOrUnknown(void* pBook) {
    void* pTab = SafeReadPtr(pBook, kOff_RightTabCtrl);
    if (!pTab) {
        return kTabUnknown;
    }
    const int t = SafeReadInt(pTab, kOff_TabSelected);
    return (t >= 0 && t <= kRightTabNone) ? t : kTabUnknown;
}

// -------------------------------------------------------------------------------------------------
// I1.2 -- THE SCREEN SPLIT, and the only two questions anybody is allowed to ask about it.
//
// The ownership rule's screen half: the item row + its magnifier exist ONLY on left tab 0..8, the
// "search cards per level" control ONLY on left tab 9, and an UNKNOWN answer paints NEITHER. These
// two predicates are exhaustive and mutually exclusive by construction -- `OnTierScreen(b)` and
// `OnHomeScreen(b)` can never both be true, and both are false when the tab control cannot be read.
// Every paint, click and tick path in this file goes through one of them; nothing else may test the
// left tab for a gating decision.
// -------------------------------------------------------------------------------------------------
bool OnTierScreen(void* pBook) {
    const int t = LeftTabOrUnknown(pBook);
    return t >= 0 && t < kTabNoCard;
}

bool OnHomeScreen(void* pBook) {
    return LeftTabOrUnknown(pBook) == kTabNoCard;
}


void MarkDirty(void* pBook, int part) {
    if (pBook && part >= 0 && part < 3) {
        SafeWriteInt(pBook, kOff_PaneDirty + static_cast<uint32_t>(part) * 8u, 1);
    }
}

void MarkAllDirty(void* pBook) {
    MarkDirty(pBook, 0);
    MarkDirty(pBook, 1);
    MarkDirty(pBook, 2);
}

// Reads RECT[i] out of a rect table and refuses anything degenerate, so a table the client has not
// filled in yet can never place a blit at a garbage coordinate.
bool ReadRect(void* pBook, uint32_t tableOff, int i, RECT& out) {
    const uint32_t off = tableOff + static_cast<uint32_t>(i) * 16u;
    out.left = SafeReadInt(pBook, off + 0);
    out.top = SafeReadInt(pBook, off + 4);
    out.right = SafeReadInt(pBook, off + 8);
    out.bottom = SafeReadInt(pBook, off + 12);
    if (out.right <= out.left || out.bottom <= out.top) {
        return false;
    }
    return out.left >= 0 && out.top >= 0 && out.right <= 1024 && out.bottom <= 1024;
}

bool PtIn(int x, int y, int l, int t, int w, int h) {
    return x >= l && x < l + w && y >= t && y < t + h;
}

// =================================================================================================
// G0.1 -- ONE text input, moved between the rows
// =================================================================================================
// The book's own CCtrlEdit. The ZRef lives at book+0xAE4 and the object pointer at book+0xAE8 --
// exactly what OnMouseButton's head reads (`mov ecx,[esi+0xae4]` with esi == book+4).
void* StockEdit(void* pBook) {
    return SafeReadPtr(pBook, kOff_EditCtrl);
}

std::string ReadEditText(void* pBook) {
    char buf[kMaxQueryLen + 8];
    SafeReadCString(StockEdit(pBook), kOff_Ctrl_Text, buf, static_cast<int>(sizeof(buf)));
    return std::string(buf);
}

bool CallEditUpdateCaret(void* pEdit); // K3.3, defined with the caret block below

// Push a string into the edit through the engine's own ZXString assignment (the same allocator and
// the same 0xC-byte header the client uses) and put the caret / selection / scroll into the state
// the client's own Home/End helpers produce (0x004CB784..0x004CB797).
bool WriteEditText(void* pBook, const std::string& text) {
    void* pEdit = StockEdit(pBook);
    if (!pEdit) {
        return false;
    }
    try {
        ZXString<char>* pStr =
                reinterpret_cast<ZXString<char>*>(reinterpret_cast<char*>(pEdit) + kOff_Ctrl_Text);
        *pStr = text.c_str();
    } catch (...) {
        return false;
    }
    SafeWriteInt(pEdit, kOff_Ctrl_Caret, static_cast<int>(text.size()));
    SafeWriteInt(pEdit, kOff_Ctrl_SelAnchor, -1);
    SafeWriteInt(pEdit, kOff_Ctrl_HScroll, 0);
    // K3.3 -- COMMIT the hand-written caret index the way the engine does. `CCtrlEdit::SetText`
    // (0x004CC512) is these same four writes followed by `call 0x004CB829` at 0x004CC548, and that
    // call is what recomputes `[ctrl+0x58]` (the measured pixel x of text[0..idx)) and re-places the
    // bar. Without it the ENGINE caret keeps the pixel x of the string it used to hold, which is
    // visible on row 0 where the engine draws the caret. The variant used here is `CreateCtrl`'s
    // (0, 0, 1) rather than `SetText`'s (0, 1, 0): both re-measure and re-place, but (0,1,0) forces
    // `[ctrl+0x5C] = [ctrl+0x48]` at 0x004CBC7B and would overwrite the -1 ("no selection", the
    // value 0x004CB6E9 uses and the value 0x004CB005 tests for) written two lines up.
    CallEditUpdateCaret(pEdit);
    return true;
}

// The text a row currently holds: the live control for the hosting row, the stash for the other.
std::string RowText(void* pBook, int row) {
    if (row == g_st.editRow) {
        return ReadEditText(pBook);
    }
    return g_st.rowText[row];
}

// =================================================================================================
// H1.1 -- HOSTING IS A READBACK, NOT A FLAG
//
// The single source of truth for "which row is the edit on" is the position the engine reports for
// the control's own IWzVector2D (ctrl+0x18). Three independent client paths key off that same
// vector, which is exactly why nothing here may key off anything else:
//
//   * the CARET: `CCtrlEdit`'s caret pass takes its x from `get_rx(ctrl+0x18)` (0x004CAFDD ->
//     0x00425AEF, `[vt+0x68]`) and its y from `get_ry(ctrl+0x18)` (0x004CB729 -> 0x00425B16,
//     `[vt+0x70]`, plus insetY at ctrl+0x3C). BOTH axes follow the vector -- so a caret drawn on
//     the mob row is PROOF the control is on the mob row.
//   * CWnd::HitTest routes a click to a child by reading the same two getters (0x009E01E7).
//   * CCtrlEdit::Draw is handed the position its owner computed from the same vector.
//
// So the readback below decides, and a move that did not take leaves `editRow` alone and gets
// re-tried from the tick. The rows are 20 px apart, so a +-4 px window cannot alias them.
// =================================================================================================
constexpr int kRowMatchSlack = 4;

// -1 when the control is unreadable or parked somewhere that is neither row.
int PhysicalRow(void* pBook, int* outX, int* outY) {
    int x = -9999, y = -9999;
    const bool ok = CtrlReadPos(StockEdit(pBook), &x, &y);
    if (outX) {
        *outX = x;
    }
    if (outY) {
        *outY = y;
    }
    if (!ok) {
        return -1;
    }
    // K2.1: the x matters now. The home screen PARKS this control 3000 px to the right so the level
    // dropdown may own its rect, and a park sitting at the row-1 y would otherwise still read as
    // "hosting row 1" and let the tick move it back on screen every frame.
    const int dx = x - kFieldX;
    if (dx < -kRowMatchSlack || dx > kRowMatchSlack) {
        return -1;
    }
    for (int row = 0; row < 2; ++row) {
        const int dy = y - kFieldY[row];
        if (dy >= -kRowMatchSlack && dy <= kRowMatchSlack) {
            return row;
        }
    }
    return -1;
}

// -------------------------------------------------------------------------------------------------
// K3.3 -- CARET GEOMETRY, LOGGED.
//
// What the disassembly says the edit derives its glyphs from, read instruction by instruction:
//   * `CCtrlEdit::Draw` (0x004CA700) takes (x, y) from its caller and uses them ONLY for the
//     background fill (0x004CA764) and for the clip rect it hands to 0x009E0C1F
//     (0x004CA7D5..0x004CA80B: {x+[ctrl+0x38], y+[ctrl+0x3C], ...}).
//   * the glyph pass it then calls (0x004CB6CF) recomputes the position from the CONTROL VECTOR:
//     `lea ecx,[esi+0x18] / call 0x00432BFA / call 0x00425B16` -> `get_ry`, `+ [ctrl+0x3C]`
//     (0x004CB71B..0x004CB72E), and inside 0x004CAF93 the x is
//     `get_rx(ctrl+0x18) + [ctrl+0x38] - [ctrl+0x60]` (0x004CAFDD..0x004CAFF0).
//   * the selection band is drawn at `y + [ctrl+0x44] - [ctrl+0x3C]` (0x004CB1E1..0x004CB1FB).
//   * the book's own CREATEPARAM (0x004C8D5F) leaves insetX `[param+4] = 0`, insetY
//     `[param+8] = -2`, `[param+0xC] = 0` and `[param+0x10] = 0`, which land at ctrl+0x38 / +0x3C /
//     +0x40 / +0x44 (0x004CA2AD..0x004CA2CF).
// Every one of those is the vector -- which is why round 11 concluded that nothing in the DRAW
// passes explains a caret a whole 20 px row pitch low.
//
// ROUND 12 ANSWER: it is not in a draw pass. It is `CCaret`'s two baked base dwords at
// `ctrl+0x94+0x00 / +0x04`, written once by `CCtrlEdit::CreateCtrl` (0x004CA575..0x004CA5B7) and
// never again -- see the full derivation above `kAddr_CCtrlEdit_UpdateCaret`. `SyncEditCaret` below
// is the fix; this logger stays because it is still the cheapest proof that the fix took: after a
// row switch the line must read `vec=(49,10)` AND `caretBase=(49,8)`.
// -------------------------------------------------------------------------------------------------

// The y offset, measured from the control's own origin, at which the caret bar is placed.
//
// STOCK uses `[ctrl+0x44]` (CREATEPARAM+0x10 == 0) for both the caret base (0x004CA586) and the
// selection band (0x004CB1E1), while the GLYPHS are drawn at `[ctrl+0x3C]` (CREATEPARAM+0x08 ==
// **-2**, 0x004CB71B..0x004CB72E). The stock bar therefore hangs two pixels below the text cell it
// belongs to -- which is exactly K3.3's "row 1's caret is a few px off-centre", and it is the same
// two pixels on both rows because it is one CREATEPARAM.
//
// This module anchors the caret to the GLYPH inset instead, so the bar covers the text cell it is
// sitting in. Both carets go through this one function -- the engine's (whose base `SyncEditCaret`
// now writes) and the module-drawn row-1 one -- so the two rows cannot drift apart. Reverting to
// byte-for-byte stock is one identifier: read `kOff_Ctrl_SelRectY` here instead of the text inset.
int CaretInsetY(void* pEdit) {
    int iy = pEdit ? SafeReadInt(pEdit, kOff_Ctrl_TextInsetY) : -2;
    if (iy < -8 || iy > 16) {
        iy = -2; // the book's CREATEPARAM value; an absurd read never moves the caret off the field
    }
    return iy;
}

// `CCtrlEdit::UpdateCaret(0, 0, 1)` -- the engine's own "re-measure the caret and put it there".
// `CreateCtrl` itself finishes with this exact call (0x004CA5DE..0x004CA5E7, after writing the caret
// index into `ctrl+0x48` the same way `WriteEditText` does), so it is the client-sanctioned way to
// commit a caret index this module set by hand. arg2 = 1 keeps the selection anchor (0x004CBC75
// only collapses `ctrl+0x5C` when it is 0). Isolated in its own function because MSVC forbids
// `__try` beside C++ unwinding.
bool CallEditUpdateCaret(void* pEdit) {
    if (!pEdit) {
        return false;
    }
    __try {
        reinterpret_cast<void(__thiscall*)(void*, int, int, int)>(kAddr_CCtrlEdit_UpdateCaret)(
                pEdit, 0, 0, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// -------------------------------------------------------------------------------------------------
// K3.3 THE FIX -- re-bake the caret's base coordinates onto wherever the control now is.
//
// Idempotent and self-checking: it reads the control's live vector, computes the base `CreateCtrl`
// WOULD have baked had the control been created there, and returns early when that is already what
// the caret holds. Safe to call from every path that moves (or might have moved) the edit; only a
// real change writes, calls the engine and logs.
//
// Ordering: the two dwords are written FIRST, then `UpdateCaret` is called -- `CCaret::SetPos` reads
// the base at 0x004C9185/0x004C918B, so a call made before the write would just re-place the caret
// at the stale row.
// -------------------------------------------------------------------------------------------------
bool SyncEditCaret(void* pEdit, const char* why) {
    if (!pEdit) {
        return false;
    }
    void* pCaret = reinterpret_cast<char*>(pEdit) + kOff_Ctrl_Caret_Obj;
    if (SafeReadInt(pCaret, kOff_Caret_Alive) == 0) {
        // `CCaret::SetPos` refuses to run in this state too (0x004C90F7), and the ctor will bake the
        // base from the live vector the moment the control is created -- nothing to do, nothing broken.
        return false;
    }
    int x = -9999, y = -9999;
    if (!CtrlReadPos(pEdit, &x, &y)) {
        return false;
    }
    const int wantX = x + SafeReadInt(pEdit, kOff_Ctrl_CaretBaseX); // 0x004CA598
    const int wantY = y + CaretInsetY(pEdit);                       // 0x004CA586, glyph-aligned
    const int haveX = SafeReadInt(pCaret, kOff_Caret_BaseX);
    const int haveY = SafeReadInt(pCaret, kOff_Caret_BaseY);
    if (haveX == wantX && haveY == wantY) {
        return false; // already correct -- no write, no engine call, no log spam
    }
    const bool wrote = SafeWriteInt(pCaret, kOff_Caret_BaseX, wantX)
            && SafeWriteInt(pCaret, kOff_Caret_BaseY, wantY);
    const bool placed = wrote ? CallEditUpdateCaret(pEdit) : false;
    return wrote;
}

// SYNCHRONOUS row switch (6.G G0.1). Runs inside the mouse hook, in the same message that produced
// the click -- which is the whole difference from round 4, where it was queued to the next flush
// and the click had therefore already been dispatched against the old position.
//
// Round 7 change: the move is only COMMITTED (stash swap, text swap, `editRow`) once the engine
// says the control actually landed on the target row. A refused move arms `wantRow` and the tick
// keeps trying; nothing about the module's idea of which row owns the typed text ever moves ahead
// of the caret again.
bool MoveEditToRow(void* pBook, int row, bool bFocus) {
    if (row < 0 || row > 1) {
        return false;
    }
    void* pEdit = StockEdit(pBook);
    if (!pEdit) {
        return false;
    }

    int beforeX = -9999, beforeY = -9999;
    const int fromRow = PhysicalRow(pBook, &beforeX, &beforeY);
    if (fromRow == row) {
        // Already physically there. Re-sync the bookkeeping if it had drifted, then just focus.
        if (g_st.editRow != row) {
            g_st.editRow = row;
        }
        g_st.wantRow = -1;
        // K3.3: no-op unless the base has drifted (a previous round, a park, a re-created control),
        // so this costs one comparison on the common path and still self-heals.
        SyncEditCaret(pEdit, "move-noop");
        if (bFocus) {
            FocusCtrl(pEdit);
        }
        return true;
    }

    // Stash what the row we are LEAVING holds. `fromRow` is the physical answer; if the control is
    // parked off both rows we fall back to the bookkeeping so no text is dropped on the floor.
    const int leaving = (fromRow >= 0) ? fromRow : g_st.editRow;
    g_st.rowText[leaving] = ReadEditText(pBook);

    const bool moved = CtrlMoveTo(pEdit, kFieldX, kFieldY[row]);
    int gotX = -9999, gotY = -9999;
    const int landed = PhysicalRow(pBook, &gotX, &gotY);

    if (landed != row) {
        // REFUSED. Do NOT flip editRow -- that is the round-6 defect. The control still holds
        // `leaving`'s text, which is where it physically is, so the state stays coherent.
        g_st.wantRow = row;
        ++g_st.wantRowTries;
        g_st.wantRowNextTry = GetTickCount() + 60;
        MarkDirty(pBook, 0);
        // The move was refused, but the vector may still have shifted -- bind the caret to wherever
        // the control ACTUALLY is, never to where it was asked to go.
        SyncEditCaret(pEdit, "move-refused");
        return false;
    }

    const bool wrote = WriteEditText(pBook, g_st.rowText[row]);
    // K3.3 -- THE CARET FOLLOWS. `WriteEditText` has just put the new row's text and its caret index
    // into `ctrl+0x34/+0x48` by hand; re-baking the base and letting `CCtrlEdit::UpdateCaret` place
    // the bar is exactly the pair of steps `CreateCtrl` performs at 0x004CA5E4/0x004CA5E7. Done
    // BEFORE the focus hand-off so the engine's focus path already sees the corrected base.
    const bool caretFixed = SyncEditCaret(pEdit, "move-ok");
    const int oldRow = g_st.editRow;
    g_st.editRow = row;
    g_st.wantRow = -1;
    g_st.wantRowTries = 0;
    // Never focus a hidden field (the cover parks the edit back on row 1 while every control is
    // hidden) -- a focused invisible edit would swallow the player's keys.
    //
    // K3.3: the focus is RE-TAKEN, not merely asserted. `CWndMan::SetFocus` returns immediately when
    // the control it is handed is already the focus (0x009E32DE), so on a row switch the engine
    // would never run its focus-entry path again -- and "a position cached at focus time" is
    // precisely the caret hypothesis this round is asked to close out. Handing the focus back to the
    // window first (the book's own `[book+0x64]=0 ; SetFocus(book+4)`, 0x008621B9) and then to the
    // edit forces that path to run at the NEW position. Both calls are the engine's own.
    if (bFocus) {
        FocusWindowSelf(pBook);
    }
    const bool focused = bFocus ? FocusCtrl(pEdit) : false;


    // Both rows repaint: the one we left shows its stashed text on the baked box, the one we
    // arrived on hands its box back to the live control.
    MarkDirty(pBook, 0);
    return true;
}

// Per-tick reconciliation (H1.1). Two jobs, both cheap:
//   1. adopt the engine's answer whenever it disagrees with the bookkeeping -- the control's text
//      belongs to wherever the caret is, full stop;
//   2. re-drive a move that was refused, so the player never has to click twice.
void ReconcileEditRow(void* pBook) {
    int x = -9999, y = -9999;
    const int phys = PhysicalRow(pBook, &x, &y);

    if (phys >= 0 && phys != g_st.editRow) {
        g_st.editRow = phys;
        MarkDirty(pBook, 0);
    }

    if (g_st.wantRow < 0) {
        return;
    }
    if (g_st.wantRow == g_st.editRow) {
        g_st.wantRow = -1;
        g_st.wantRowTries = 0;
        return;
    }
    const DWORD now = GetTickCount();
    if (g_st.wantRowNextTry != 0 && now < g_st.wantRowNextTry) {
        return;
    }
    const int want = g_st.wantRow;
    g_st.wantRow = -1; // MoveEditToRow re-arms it if it fails again
    MoveEditToRow(pBook, want, true);
}

// -------------------------------------------------------------------------------------------------
// K2.1 -- PARK THE STOCK EDIT WHILE THE HOME SCREEN IS UP.
//
// The level dropdown has to start flush under its box (spec K2.1, "no vertical gap"), and flush
// means it covers window y 26..109 -- straight through the stock edit's rect (49,30)-(169,45).
// `CWnd::HitTest` (0x009E01E7) never consults a control's shown flag (see the kLvlList* block), so
// the hidden edit would swallow the clicks for the first two dropdown rows. The strip already does
// exactly this for `BtSearch2`; the edit is the same problem and gets the same, engine-native lever.
//
// `editParked` is set ONLY from a read-back, and `LevelListY()` keys the whole geometry -- paint AND
// hit test -- on that one flag, so a park that did not take degrades to the old 20 px gap rather
// than to two dead rows.
// -------------------------------------------------------------------------------------------------
constexpr int kEditParkOffset = 3000;

void ParkStockEdit(void* pBook, bool bPark) {
    void* pEdit = StockEdit(pBook);
    if (!pEdit) {
        return;
    }
    const int homeRow = (g_st.editRow == kRowItem) ? kRowItem : kRowMob;
    const int wantX = bPark ? (kFieldX + kEditParkOffset) : kFieldX;
    const int wantY = kFieldY[homeRow];
    int curX = -9999, curY = -9999;
    const bool haveXY = CtrlReadPos(pEdit, &curX, &curY);
    if (haveXY && curX == wantX && curY == wantY) {
        SyncEditCaret(pEdit, bPark ? "park-noop" : "unpark-noop"); // no-op unless the base drifted
        if (g_st.editParked != bPark) {
            g_st.editParked = bPark;
            MarkAllDirty(pBook); // LevelListY() just changed -> the list has to be redrawn there
        }
        return;
    }
    const bool moved = CtrlMoveTo(pEdit, wantX, wantY);
    int gotX = -9999, gotY = -9999;
    const bool got = CtrlReadPos(pEdit, &gotX, &gotY);
    // K3.3: the park is a `raw_RelMove` like any other, so the caret's baked base has to travel with
    // it -- otherwise a parked (3049,y) edit leaves a blinking bar sitting on the field it vacated.
    SyncEditCaret(pEdit, bPark ? "park" : "unpark");
    const bool nowParked = bPark && got && gotX == wantX;
    if (g_st.editParked != nowParked) {
        g_st.editParked = nowParked;
        MarkAllDirty(pBook); // LevelListY() just changed -> the list has to be redrawn there
    }
}

// Where the level dropdown starts, in WINDOW space. Flush under the box only while the edit is
// provably out of the way; the round-8 safe y otherwise. Paint and hit test share this one answer.
int LevelListY() {
    return g_st.editParked ? kLvlListYFlush : kLvlListYSafe;
}

// I1.5 -- is the point inside the COLOUR TAB strip? Read from the LIVE control rather than the
// creation rect {-7, 25, 50, 305}, so a strip somebody moved is still tested where it actually is;
// the creation rect is only the fallback for an unreadable control. Declared here because
// HitFieldRow has to defer to it -- see below.
bool HitLeftTabStrip(void* pBook, int rx, int ry);

// Which field row a WINDOW-space point lands in, or -1. The width/height come from the live control
// when it is readable, so a control that somehow got a different size is never mis-tested.
//
// I1.5 -- THE STRIP ALWAYS WINS AN OVERLAP.
// The field boxes span window x 48..169; the colour tab strip's own control rect is
// {-7, 25, 50, 305}, i.e. x -7..49 -- so columns 48 and 49 of the MOB row (y 29..45) and of the
// ITEM row's bottom edge (y 25..25) are claimed by BOTH. Two owners for one pixel is exactly what
// the round-8 rule forbids, and on those columns it means a tier click is swallowed by a field row
// and never reaches the client. The strip is the older claim and the one the player cannot move, so
// it wins; the field rows give up two columns they never needed.
int HitFieldRow(void* pBook, int rx, int ry) {
    void* pEdit = StockEdit(pBook);
    int w = SafeReadInt(pEdit, kOff_Ctrl_Width);
    int h = SafeReadInt(pEdit, kOff_Ctrl_Height);
    if (w <= 0 || w > 512) {
        w = kFieldW;
    }
    if (h <= 0 || h > 128) {
        h = kFieldH;
    }
    int row = -1;
    for (int r = 0; r < 2 && row < 0; ++r) {
        // The clickable band is the BOX, not just the 120x15 control: the baked art is 122x17 and
        // the player aims at what they can see.
        if (PtIn(rx, ry, kBoxX, kBoxY[r], kBoxW, kBoxH) || PtIn(rx, ry, kFieldX, kFieldY[r], w, h)) {
            row = r;
        }
    }
    if (row >= 0 && HitLeftTabStrip(pBook, rx, ry)) {
        return -1;
    }
    return row;
}

// =================================================================================================
// WZ art -- every probe is keyed on the book GENERATION, so a miss retries on the next open
// =================================================================================================
struct CachedSprite {
    IWzCanvasPtr canvas;
    int w = 0;
    int h = 0;
    unsigned gen = 0; // generation of the last attempt; 0 = never tried
};

IWzCanvasPtr LoadSprite(const wchar_t* sUOL) {
    IWzCanvasPtr c;
    try {
        Ztl_variant_t v = get_object_or_empty(sUOL);
        if (v.vt == VT_UNKNOWN || v.vt == VT_DISPATCH) {
            c = IWzCanvasPtr(v.GetUnknown(false, false));
        }
    } catch (...) {
        c = nullptr;
    }
    return c;
}

// Returns true when `s` holds usable art. Probes AT MOST ONCE PER BOOK, and only while it has none.
bool ResolveSprite(CachedSprite& s, const wchar_t* sUOL, int minW, int maxW, int minH, int maxH,
        const char* tag) {
    if (s.canvas) {
        return true;
    }
    if (s.gen == g_bookGen) {
        return false; // already tried on THIS book; the next OnCreate re-arms it
    }
    s.gen = g_bookGen;
    s.canvas = LoadSprite(sUOL);
    if (!s.canvas) {
        return false;
    }
    unsigned w = 0, h = 0;
    bool ok = false;
    try {
        ok = SUCCEEDED(s.canvas->get_width(&w)) && SUCCEEDED(s.canvas->get_height(&h));
    } catch (...) {
        ok = false;
    }
    if (!ok || w == 0 || h == 0 || w > 4096u || h > 4096u) {
        s.canvas = nullptr;
        return false;
    }
    s.w = static_cast<int>(w);
    s.h = static_cast<int>(h);
    if (s.w < minW || s.w > maxW || s.h < minH || s.h > maxH) {
        s.canvas = nullptr;
        return false;
    }
    return true;
}

CachedSprite g_box[2][2]; // [row][variant]
CachedSprite g_iconBase;

const char* BoxTag(int row, int variant) {
    static const char* kTags[2][2] = { { "item/box0", "item/box1" }, { "mob/box0", "mob/box1" } };
    return kTags[row][variant];
}

bool ResolveBox(int row, int variant) {
    // 122x17 by contract; a couple of pixels of drift still rings the 120x15 control, but a canvas
    // of a wildly different size is treated as "no art" rather than blitted over the page.
    return ResolveSprite(g_box[row][variant], kUOL_Box[row][variant], kFieldW, kFieldW + 16, kFieldH,
            kFieldH + 16, BoxTag(row, variant));
}

// The text font for the NON-hosting row's stashed text. Same recipe monsterBookDrops.cpp uses for
// its % labels, which is proven in game -- and re-armed per book like everything else.
IWzFontPtr g_font;
unsigned g_fontGen = 0;
constexpr unsigned kTextFg = 0xFF000000; // black, the same colour the stock edit's CREATEPARAM sets
constexpr unsigned kFontHeight = 11;

IWzFont* EnsureFont() {
    if (g_font) {
        return g_font;
    }
    if (g_fontGen == g_bookGen) {
        return nullptr;
    }
    g_fontGen = g_bookGen;
    try {
        PcCreateObject<IWzFontPtr>(L"Canvas#Font", g_font, nullptr);
        if (g_font) {
            Ztl_bstr_t sName(L"Dotum");
            if (FAILED(g_font->raw_Create(sName, kFontHeight, kTextFg, vtEmpty))) {
                g_font = nullptr;
            }
        }
    } catch (...) {
        g_font = nullptr;
    }
    return g_font;
}

// ROUND 11 -- the FIELD ROWS' font, and only theirs: Arial 12 black, byte-for-byte what the stock
// `CCtrlEdit` builds for itself (see kRowFontFace above). A row's text has to look identical whether
// the module drew it (stash, or row 1 while hosting) or the control did (row 0 while hosting), and
// the caret's x is measured with the same font the glyphs were drawn with or it lands between
// characters. Falls back to the Dotum body font if the face is unavailable, so a client without
// Arial degrades to the previous look rather than to no text at all.
IWzFontPtr g_rowFont;
unsigned g_rowFontGen = 0;
IWzFont* EnsureFont();

IWzFont* EnsureRowFont() {
    if (g_rowFont) {
        return g_rowFont;
    }
    if (g_rowFontGen == g_bookGen) {
        return EnsureFont(); // already tried this book -- fall back rather than retry per frame
    }
    g_rowFontGen = g_bookGen;
    try {
        PcCreateObject<IWzFontPtr>(L"Canvas#Font", g_rowFont, nullptr);
        if (g_rowFont) {
            Ztl_bstr_t sName(kRowFontFace);
            if (FAILED(g_rowFont->raw_Create(sName, kRowFontHeight, kTextFg, vtEmpty))) {
                g_rowFont = nullptr;
            }
        }
    } catch (...) {
        g_rowFont = nullptr;
    }
    return g_rowFont ? static_cast<IWzFont*>(g_rowFont) : EnsureFont();
}

// H1.10's placeholder wants the same grey the baked boxes use. Same recipe, same per-book re-arm;
// a failure here is cosmetic (the caller falls back to the black font), never fatal.
IWzFontPtr g_hintFont;
unsigned g_hintFontGen = 0;
constexpr unsigned kHintFg = 0xFF9A9A9A;

IWzFont* EnsureHintFont() {
    if (g_hintFont) {
        return g_hintFont;
    }
    if (g_hintFontGen == g_bookGen) {
        return nullptr;
    }
    g_hintFontGen = g_bookGen;
    try {
        PcCreateObject<IWzFontPtr>(L"Canvas#Font", g_hintFont, nullptr);
        if (g_hintFont) {
            Ztl_bstr_t sName(L"Dotum");
            if (FAILED(g_hintFont->raw_Create(sName, kFontHeight, kHintFg, vtEmpty))) {
                g_hintFont = nullptr;
            }
        }
    } catch (...) {
        g_hintFont = nullptr;
    }
    return g_hintFont;
}

// =================================================================================================
// canvases
// =================================================================================================
IWzCanvasPtr CanvasOfLayer(IWzGr2DLayer* pLayer) {
    IWzCanvasPtr out;
    if (!pLayer) {
        return out;
    }
    try {
        IWzCanvas* raw = nullptr;
        if (FAILED(pLayer->get_canvas(vtEmpty, &raw)) || !raw) {
            return out;
        }
        out.Attach(raw); // get_canvas hands back an AddRef'd pointer
    } catch (...) {
        return IWzCanvasPtr();
    }
    return out;
}

// =================================================================================================
// J-C -- THE WINDOW CANVAS. Ask the ENGINE, do not re-derive it.
//
// Round 8 hand-rolled the choice (overlab +0x20 else layer +0x18) and then called
// `get_canvas(vtEmpty)` on the winner. `CWnd::GetCanvas` (0x00425C4C) makes the SAME layer choice
// but asks for canvas **index 0** (`Ztl_variant_t(0, VT_I4)`), and `CWnd::Draw` blits `backgrnd`
// into whatever GetCanvas returns -- so index 0 is, by construction, the surface the window shows.
// Two canvases of one layer is exactly the round-9 report: `PAINT row=0 variant=0 art=1` in the log
// and a stale, placeholder-less box on screen.
//
// So the engine call is the primary path and the round-8 walk is only the fallback, and both
// pointers plus both canvases are LOGGED (LogWindowCanvasChoice) so the next session's log settles
// the question with data instead of with this comment.
//
// GetCanvas THROWS `_com_error(E_POINTER)` (0x00425C7E / 0x00425CC1 -> 0x00A5FDE4) when neither
// layer exists, so it needs the file's usual two-layer net: C++ `catch(...)` for the throw, SEH
// around that for a hard fault. Neither frame may hold a smart pointer, hence the raw `void*`.
// =================================================================================================
void* WindowCanvasRawInner(void* pBook) {
    void* raw = nullptr;
    try {
        reinterpret_cast<void*(__thiscall*)(void*, void**)>(kAddr_CWnd_GetCanvas)(pBook, &raw);
    } catch (...) {
        return nullptr; // "this window has no layer yet" -- an ordinary state, not an error
    }
    return raw;
}

void* WindowCanvasRawGuarded(void* pBook) {
    if (!pBook) {
        return nullptr;
    }
    // Nothing to ask for until at least one layer exists; asking anyway would only make GetCanvas
    // raise E_POINTER and cost a first-chance exception every frame.
    if (!SafeReadPtr(pBook, kOff_WndOverlab) && !SafeReadPtr(pBook, kOff_WndLayer)) {
        return nullptr;
    }
    __try {
        return WindowCanvasRawInner(pBook);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The WINDOW's own canvas -- a private buffer, not the WZ node.
IWzCanvasPtr WindowCanvas(void* pBook) {
    IWzCanvasPtr out;
    void* raw = WindowCanvasRawGuarded(pBook);
    if (raw) {
        // `0x00425D2E` stores the AddRef'd pointer `raw_get_canvas` returned straight into the slot
        // (0x00425D6C) and CWnd::Draw Releases it afterwards (0x009E05AA), so this owns a reference.
        out.Attach(static_cast<IWzCanvas*>(raw));
        return out;
    }
    // FALLBACK: the round-8 walk. Kept so a client whose GetCanvas refuses still gets a surface --
    // and so the log can show the two answers side by side when they disagree.
    void* pLayer = SafeReadPtr(pBook, kOff_WndOverlab);
    if (!pLayer) {
        pLayer = SafeReadPtr(pBook, kOff_WndLayer);
    }
    return CanvasOfLayer(reinterpret_cast<IWzGr2DLayer*>(pLayer));
}

// J-C's instrumentation, in one line: both layer pointers, the canvas each of them answers with for
// vtEmpty (what round 8 painted on) and the canvas CWnd::GetCanvas answers with (what CWnd::Draw
// paints `backgrnd` on, i.e. what the screen shows). If `engine=` ever differs from the `cv=` of the
// layer it came from, the ghost strip is explained on that one line.
void LogWindowCanvasChoice(void* pBook, const char* why) {
    void* ov = SafeReadPtr(pBook, kOff_WndOverlab);
    void* ly = SafeReadPtr(pBook, kOff_WndLayer);
    IWzCanvasPtr cOv = CanvasOfLayer(reinterpret_cast<IWzGr2DLayer*>(ov));
    IWzCanvasPtr cLy = CanvasOfLayer(reinterpret_cast<IWzGr2DLayer*>(ly));
    void* eng = WindowCanvasRawGuarded(pBook);
    if (eng) {
        // WindowCanvasRawGuarded handed back an owning reference; this probe must not keep it.
        IWzCanvasPtr drop;
        drop.Attach(static_cast<IWzCanvas*>(eng));
        return;
    }
}

IWzCanvasPtr LayerCanvas(void* pBook, uint32_t layerOff) {
    return CanvasOfLayer(reinterpret_cast<IWzGr2DLayer*>(SafeReadPtr(pBook, layerOff)));
}

// =================================================================================================
// the card index -- every record the open book owns, addressed as (tab, page, index)
//
// Walks exactly the arrays GetSlotCard walks, so every entry is guaranteed resolvable by the
// original later. Card record layout, from LoadCardList (0x00684B55 / 0x00684BA2) and
// SetSelectedCard (0x00867969 / 0x00867979): { +0 cardId, +4 mobId, +8 tab, +0xC ordinal }.
// =================================================================================================
constexpr int kMaxPagesPerTab = 256;
constexpr int kMaxEntriesPerPage = 64;

bool BuildCardIndex(void* pBook) {
    if (!pBook) {
        return false;
    }
    if (g_cardsForBook == pBook && !g_cards.empty()) {
        return true;
    }

    g_cards.clear();
    g_lower.clear();
    g_cardsForBook = pBook;

    for (int tab = 0; tab < kTabCount; ++tab) {
        void* pPages = SafeReadPtr(pBook, kOff_TabPages + static_cast<uint32_t>(tab) * 4u);
        const int nPages = SafeArrayCount(pPages);
        if (nPages <= 0 || nPages > kMaxPagesPerTab) {
            continue;
        }
        for (int p = 0; p < nPages; ++p) {
            void* pEntries = SafeIndexPtr(pPages, p);
            const int nEntries = SafeArrayCount(pEntries);
            if (nEntries <= 0 || nEntries > kMaxEntriesPerPage) {
                continue;
            }
            for (int i = 0; i < nEntries; ++i) {
                // stride 8, record at +4 -- GetSlotCard's `[eax + edx*8 + 4]` (0x00867AA0)
                void* pRec = SafeIndexPtr(pEntries, i * 2 + 1);
                if (!pRec) {
                    continue;
                }
                CardLoc loc;
                loc.tab = tab;
                loc.page = p;
                loc.index = i;
                loc.cardId = SafeReadInt(pRec, 0);
                loc.mobId = SafeReadInt(pRec, 4);
                if (loc.cardId <= 0 || loc.mobId <= 0) {
                    continue;
                }
                g_cards.push_back(loc);
            }
        }
    }

    if (g_cards.empty()) {
        g_cardsForBook = nullptr; // no negative caching: retry on the next call
        return false;
    }

    // Names: String/Mob.img in one pass, GetMobNameById as the fallback. ~860 lookups, so this
    // happens on the FLUSH only -- never on a draw or input path.
    g_lower.reserve(g_cards.size());
    IWzPropertyPtr pMobStr;
    try {
        Ztl_variant_t v = get_object_or_empty(L"String/Mob.img");
        pMobStr = v.GetUnknown();
    } catch (...) {
        pMobStr = nullptr;
    }
    for (const CardLoc& c : g_cards) {
        std::string name;
        if (pMobStr) {
            try {
                wchar_t key[16];
                _snwprintf_s(key, _countof(key), _TRUNCATE, L"%d", c.mobId);
                Ztl_variant_t vMob = get_item_or_empty(pMobStr, key);
                IWzPropertyPtr pMob = vMob.GetUnknown();
                if (pMob) {
                    Ztl_variant_t vName = get_item_or_empty(pMob, L"name");
                    if (vName.vt == VT_BSTR && vName.bstrVal) {
                        _bstr_t t(vName.bstrVal);
                        const char* s = static_cast<const char*>(t);
                        if (s) {
                            name = s;
                        }
                    }
                }
            } catch (...) {
            }
        }
        if (name.empty()) {
            try {
                std::string n = GetMobNameById(c.mobId);
                if (n != "Unknown") {
                    name = n;
                }
            } catch (...) {
            }
        }
        g_lower.push_back(ToLower(name));
    }

    g_st.cardsBuilt = true;
    return true;
}

int FindCardByMob(int mobId) {
    for (size_t i = 0; i < g_cards.size(); ++i) {
        if (g_cards[i].mobId == mobId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// =================================================================================================
// result views
// =================================================================================================
// K7(b) -- how many of the grid's 25 slots THIS result view uses per page. One function, because
// four separate places used to spell `page * 25 + slot` and the stride now depends on the view: a
// dropper page is 20 so its bottom row of labels has somewhere to go (see kDropperGridSlots).
// Everything that maps between (page, slot) and a result index goes through here or through
// ResultCardIndexAt, so a page count and a slot lookup can never disagree.
int ResultSlotsPerPage() {
    return g_st.cardFromDroppers ? kDropperGridSlots : kGridSlots;
}

int CardPageCount() {
    const int n = static_cast<int>(g_st.cardResults.size());
    const int per = ResultSlotsPerPage();
    return (n <= 0) ? 1 : ((n + per - 1) / per);
}

int ItemPageCount() {
    const int n = static_cast<int>(g_st.itemResults.size());
    return (n <= 0) ? 1 : ((n + kItemSlots - 1) / kItemSlots);
}

void SetItemResults(std::vector<int>&& ids, const std::string& query) {
    g_st.itemResults = std::move(ids);
    g_st.itemPage = 0;
    g_st.itemQuery = query;
    g_st.itemView = !g_st.itemResults.empty();
}

void ClearItemView(void* pBook, const char* reason) {
    if (!g_st.itemView && g_st.itemResults.empty() && g_st.itemQuery.empty()) {
        return;
    }
    g_st.itemView = false;
    g_st.itemResults.clear();
    g_st.itemPage = 0;
    g_st.itemQuery.clear();
    if (pBook) {
        MarkAllDirty(pBook);
    }
}

// I1.6: mobId -> the searched item's ppm for that mob, straight off the type-2 reply. It belongs to
// the droppers view and dies with it, or a later mob-name search would find stale percentages under
// its cards. Lives here (rather than next to its painter) so every teardown path can clear it.
std::unordered_map<int, int> g_dropperPpm;

void ClearCardView(void* pBook, const char* reason) {
    if (!g_st.cardView) {
        return;
    }
    g_st.cardView = false;
    g_st.cardResults.clear();
    g_st.cardPage = 0;
    g_st.cardFromDroppers = false;
    g_st.dropperItem = 0;
    g_dropperPpm.clear();
    if (pBook) {
        SafeWriteInt(pBook, kOff_CardPage, 0);
        MarkAllDirty(pBook);
    }
}

// Defined with the navigation watcher, far below: OUR OWN tab moves must not be read as the
// player's, or the watcher tears the view down on the frame it opened.
void RebaselineTabs(void* pBook);

// =================================================================================================
// K8 -- A RESULT VIEW OPENS ON ITS FIRST RESULT
//
// The book keeps its own "selected card" in (`[+0xAD8]+0x34` tab, `+0x5B0` page, `+0x5B8` index),
// and until now nothing pointed it at a result: `OpenCardView` wrote the two indices and dirtied the
// panes, so the grid showed the filtered list while the RIGHT page still showed whatever the client
// had selected last. In the shipped log that is
//     [00:14:57.454] LEVEL search level=1 probed=1115 -> 4 hits
//     [00:14:57.552] DROPS SETMOBINFO mob=100100 cardLevel=0 art=1 gate=patched   <- Snail
//     [00:14:57.553] LEVEL tier switched to 0 (readback=0)
//     [00:14:57.553] VIEW card ON reason=level-search hits=4 pages=1
// with no further SETMOBINFO -- the tier-0 first card, exactly what 6.K-K8 reports. (That SETMOBINFO
// comes from `SwitchColourTier`: `CCtrlTab::SetSelected` notifies the parent synchronously,
// 0x008606AF -> OnChildNotify(0x7D6,0x1F4) at 0x00861FD0, which zeroes the page/selection/list
// indices and then calls `SelectCurrentSlot` at 0x00862002 -- all of it while `cardView` is still
// false, so the remap is not in play yet.)
//
// The fix is at that root, not per search: once the view state is up and (+0x5B0, +0x5B8) are (0,0),
// the client's own `SelectCurrentSlot` asks GetSlotCard(tab, 0, 0), THIS module's remap answers with
// `cardResults[0]`, and the client's own SetSelectedCard/SetMobInfo do the rest. Mob search, level
// search and droppers all go through `OpenCardView`, so all three get it -- which is what K8 means
// by "K5.2 and K8 must be answered by the same mechanism".
// =================================================================================================
void SelectFirstResult(void* pBook, const char* reason) {
    if (!pBook || !g_st.cardView || g_st.cardResults.empty()) {
        // Round 11: this was one of the round-10 additions that could refuse silently. K8 failing
        // looks exactly like K8 never having been written, so it says so.
        return;
    }
    const int ci = g_st.cardResults[0];
    const int mobId = (ci >= 0 && ci < static_cast<int>(g_cards.size()))
            ? g_cards[static_cast<size_t>(ci)].mobId
            : 0;
    // `OnChildNotify` itself refuses to select on the cover (0x00861FFA `cmp [eax+0x34],9 / je`),
    // and Draw diverts there anyway (0x0086590E) -- so neither do we.
    const int tab = LeftTabOrUnknown(pBook);
    if (tab == kTabUnknown || tab == kTabNoCard) {
        return;
    }
    // The remap answers (page 0, index 0) with cardResults[0] only if the book is ASKING for (0,0).
    SafeWriteInt(pBook, kOff_CardPage, 0);
    SafeWriteInt(pBook, kOff_SelSlot, 0);
    const bool ok = CallSelectCurrentSlot(pBook);
    // SetSelectedCard also NAVIGATED the book to that card's real coordinates inside its own tier
    // (`page = ord/25`, `index = ord%25`, stored at 0x0086798D / 0x008679A9). In a result view those
    // address the wrong slot entirely, so the grid coordinates go straight back to the slot the
    // first result actually occupies. The Update hook would put the PAGE back a frame later anyway
    // (its `cardView` correction), but not the selection, and not before the next repaint.
    if (ok) {
        SafeWriteInt(pBook, kOff_CardPage, 0);
        SafeWriteInt(pBook, kOff_SelSlot, 0);
    }
    // SetSelectedCard moved both tabs on our behalf (colour strip to `record+8` at 0x00867970,
    // content tab to 0 at 0x008679AE). K4's stored selection is restored first when the item view
    // owns the page, then BOTH baselines are retaken so WatchNavigation does not read our own
    // navigation as the player leaving the view and clear it.
    if (ok && g_st.itemViewLive) {
        void* pRightTab = SafeReadPtr(pBook, kOff_RightTabCtrl);
        if (pRightTab && RightTabOrUnknown(pBook) != kTabDropping) {
            CallTabSetSelected(pRightTab, kTabDropping, "k8-select/right");
        }
    }
    RebaselineTabs(pBook);
    MarkAllDirty(pBook);
}

void OpenCardView(void* pBook, std::vector<int>&& hits, const char* reason,
        bool fromDroppers = false) {
    g_st.cardResults = std::move(hits);
    if (static_cast<int>(g_st.cardResults.size()) > kMaxCardResults) {
        g_st.cardResults.resize(kMaxCardResults);
    }
    g_st.cardView = true;
    g_st.cardPage = 0;
    // K7(b): the page STRIDE depends on which view this is, so the flag is set here -- before
    // anything (this function's own log line included) asks CardPageCount() anything. It also stops
    // a mob or level search that follows a droppers view from inheriting its stride, its item and
    // its ppm table: `ClearCardView` is not on those two paths (RunMobSearchNow/RunLevelSearchNow
    // call it only via ClearAllSearchState on navigation), so the leftovers used to survive into the
    // next search and could paint stale percentages under whichever result mobs happened to match.
    g_st.cardFromDroppers = fromDroppers;
    if (!fromDroppers) {
        g_st.dropperItem = 0;
        g_dropperPpm.clear();
    }
    if (pBook) {
        SafeWriteInt(pBook, kOff_CardPage, 0);
        SafeWriteInt(pBook, kOff_SelSlot, 0);
        MarkAllDirty(pBook);
    }
    SelectFirstResult(pBook, reason); // K8
}

// Queued from the click, run on the flush (the index build is ~860 WZ lookups).
void RunMobSearchNow(void* pBook, const std::string& q) {
    if (!BuildCardIndex(pBook)) {
        return;
    }
    const std::string needle = ToLower(q);
    std::vector<int> hits;
    hits.reserve(64);
    for (size_t i = 0; i < g_cards.size() && i < g_lower.size(); ++i) {
        if (!g_lower[i].empty() && g_lower[i].find(needle) != std::string::npos) {
            hits.push_back(static_cast<int>(i));
        }
    }
    // Names that START with the needle first, then alphabetical, then by card id.
    std::sort(hits.begin(), hits.end(), [&needle](int a, int b) {
        const std::string& la = g_lower[static_cast<size_t>(a)];
        const std::string& lb = g_lower[static_cast<size_t>(b)];
        const bool pa = la.size() >= needle.size() && la.compare(0, needle.size(), needle) == 0;
        const bool pb = lb.size() >= needle.size() && lb.compare(0, needle.size(), needle) == 0;
        if (pa != pb) {
            return pa;
        }
        if (la != lb) {
            return la < lb;
        }
        return g_cards[static_cast<size_t>(a)].cardId < g_cards[static_cast<size_t>(b)].cardId;
    });
    ClearItemView(pBook, "mob-search"); // the right page goes back to the mob's own info
    OpenCardView(pBook, std::move(hits), "mob-search");
}

void ArmItemQuery(const std::string& q) {
    g_st.pendingArmed = true;
    g_st.pendingType = kQuery_ItemName;
    g_st.pendingText = q;
    g_st.pendingItem = 0;
    g_st.awaitItems = true;
    g_st.awaitDroppers = false;
    g_st.awaitSince = GetTickCount();
}

void RequestDroppers(int itemId) {
    if (itemId <= 0) {
        return;
    }
    g_st.pendingArmed = true;
    g_st.pendingType = kQuery_Droppers;
    g_st.pendingText.clear();
    g_st.pendingItem = itemId;
    g_st.awaitDroppers = true;
    g_st.awaitItems = false;
    g_st.awaitSince = GetTickCount();
}

// Run the search that belongs to a row. Both magnifiers land here; the text always comes from the
// ROW, so it does not matter which row is currently hosting the edit.
void RunRowSearch(void* pBook, int row) {
    const std::string q = Trim(RowText(pBook, row));
    if (static_cast<int>(q.size()) < kMinQueryLen) {
        if (row == kRowMob) {
            ClearCardView(pBook, "mob-query-too-short");
        } else {
            ClearItemView(pBook, "item-query-too-short");
        }
        return;
    }
    const std::string qq = q.substr(0, kMaxQueryLen);
    if (row == kRowMob) {
        g_st.mobQuery = qq;
        g_st.wantMobSearch = true; // the index build runs on the flush
    } else {
        ArmItemQuery(qq);
    }
}

// =================================================================================================
// painting
// =================================================================================================
// =================================================================================================
// K3.1 / K3.2 -- THE PIXEL OWNERSHIP RULE, NOW PER ROW AND KEYED ON THE SURFACE
//
//   | state                       | box art                       | text          | caret |
//   |-----------------------------|-------------------------------|---------------|-------|
//   | row text EMPTY              | *Box0 (the baked placeholder) | --            | edit, |
//   | row text NON-EMPTY          | *Box1                         | see below     | if it |
//                                                                                 | hosts |
//
// TWO changes from I1.1, both mandated by 6.K and both evidenced by the shipped log.
//
// K3.1 -- THE VARIANT IS A PURE FUNCTION OF THAT ROW'S TEXT, hosting or not. I1.1 gave a hosting row
// `*Box1` unconditionally, which the log shows doing exactly the wrong thing:
//     PAINT row=1 owner=edit variant=1 art=1 ... editRow=1 live="" ...
// -- an EMPTY row rendered without its placeholder, purely because the caret happened to be in it.
// So both placeholders are visible on entering a tier screen and clicking a field blanks neither;
// the placeholder disappears the moment that row's text becomes non-empty, and comes back when it
// empties again.
//
// K3.2 -- WHO DRAWS THE GLYPHS IS DECIDED BY THE SURFACE THE ROW'S BOX LANDS ON, not by hosting.
// The log names both surfaces on the same page:
//     PAINT row=0 ... canvas=223D829C     <- the WINDOW canvas (CWnd::GetCanvas, index 0)
//     PAINT row=1 ... canvas=30816F84     <- the LEFT PAGE layer's canvas
// and `CCtrlEdit::Draw` paints into the PARENT WINDOW's canvas (its first act is `call 0x004C0690`,
// whose first instruction is `mov ecx,[esi+0x24]` -- the parent). The left-page LAYER composites
// ABOVE that canvas, so:
//     row 0 (window canvas)   -> the edit's own glyphs are on the SAME surface, drawn after ours:
//                                the edit owns the text and the module must never draw it, or the
//                                two copies double (round 8, image 4).
//     row 1 (left-page layer) -> our box lands ON TOP of whatever the edit drew, so the module MUST
//                                draw the text itself while hosting, or the row looks empty as the
//                                player types.
// `kRowSurface` states that, `ModuleOwnsRowText` derives it, and every PAINT line carries the
// surface so a future log can be read without this comment.
// =================================================================================================
constexpr int kBoxVariant_Placeholder = 0;
constexpr int kBoxVariant_Empty = 1;

constexpr int kSurface_Window = 0;   // CWnd::GetCanvas(index 0) -- where CCtrlEdit::Draw also lands
constexpr int kSurface_LeftPage = 1; // book+0xB18's layer -- composites ABOVE the window canvas
constexpr int kRowSurface[2] = { kSurface_Window, kSurface_LeftPage };

const char* SurfaceName(int surface) {
    return (surface == kSurface_Window) ? "window" : "left-page";
}

// =================================================================================================
// ROUND 11 -- WHEN a window-canvas blit happens matters as much as WHERE, and the ordering is now
// stated in code and printed on every PAINT line.
//
// THE TWO FACTS, both re-derived for this round:
//
// 1. WITHIN one repaint pass, the child controls draw AFTER primary slot 11.
//    `CWnd::Draw` (0x009E0502) is the whole of the book's slot 11 (the thunk at 0x00861E8D is
//    `push [esp+4]; call 0x009E0502; ret 4`), and read end to end -- 0x009E0502..0x009E067B -- its
//    entire body is: get `backgrnd` (this+0x68), ask `CWnd::GetCanvas` (0x00425C4C) for the surface,
//    and blit it at (this+0x40, this+0x44) through `[canvasvt+0x80]` at 0x009E0587. It walks NO
//    children. `CCtrlEdit::Draw` is the SAME virtual slot on the same hierarchy -- CCtrlEdit's
//    primary vtable (0x00AF2C98) has 0x004CA700 at index 11, exactly where the book has its thunk --
//    and it takes its target canvas from the PARENT chain (0x004CA719 `call 0x004C0690`, whose first
//    instruction is `mov ecx,[esi+0x24]`). So parent and child paint the same surface, and if the
//    child pass ran first the parent's opaque `backgrnd` blit would erase every control on every
//    window in the client. Controls are visible, therefore the child pass runs SECOND -- and this
//    module's hook, which runs the instant `CWnd::Draw` returns, lands underneath them.
//
// 2. That pass is NOT per frame. It is a retained buffer written on demand.
//    Proof from the session trace the user reproduced: from 00:18:07.567 to 00:19:27.479 the book is open on
//    the home screen and the Update pump is running normally (`TICK ... L=9(tier=0 home=1) ...
//    sinceUpdate=16ms` every two seconds, forty of them), and in those EIGHTY SECONDS the
//    rate-limited-to-3s `PAINT level box` line does not appear once. The next one lands at
//    00:19:27.479, half a second before `LEVEL dropdown OPEN` -- i.e. when the player moved the
//    mouse back onto the window.
//
// TOGETHER they say: a blit from the DRAW HOOK is safe on the window canvas (the edit repaints over
// it in the same pass), and a blit from anywhere else -- the tick, the mouse hook -- lands on the
// retained buffer AFTER the edit's glyphs and stays there until the next user-driven repaint.
//
// THE REGRESSION THAT CAUSED. K3.1 made the box VARIANT follow the row's TEXT, so the first
// character typed into row 0 now flips its variant, and the tick's variant watcher called the
// window-canvas painter directly for it -- an opaque 122x17 `itemBox1` over the live glyphs, with
// `drew=""`, because row 0's text belongs to the edit. Before K3.1 a hosting row was pinned to
// variant 1 and that path could never fire, which is exactly the "it used to be visible" report.
//
// THE RULE, implemented in PaintRow: an OUT-OF-DRAW-PASS repaint of a WINDOW-surface row is refused
// while that row hosts a shown edit that has text. Nothing else changes -- the same call from the
// draw hook still paints, the level control (whose edit is parked off-screen on the home screen)
// still repaints on the spot for K2.2, and a row with no glyphs to protect still repaints for J-C.
constexpr int kPaintWhy_DrawPass = 0;  // inside the CWnd::Draw / Redraw(0) hook -- always allowed
constexpr int kPaintWhy_OutOfBand = 1; // tick / mouse hook -- the retained buffer, after the edit

const char* PaintWhyName(int why) {
    return (why == kPaintWhy_DrawPass) ? "draw-pass" : "out-of-band";
}

// Defined with the other little drawing helpers below; PaintRow's caret needs it first.
void FillRect(IWzCanvasPtr& c, int x, int y, int w, int h, unsigned color);

// True when the row's box covers the edit's own glyphs, i.e. the module has to draw them.
bool ModuleOwnsRowText(int row) {
    return row >= 0 && row < 2 && kRowSurface[row] != kSurface_Window;
}

// The text the MODULE is allowed to paint for a row: the stash when the row is not hosting, and --
// only on a surface that covers the edit -- the LIVE control text when it is.
std::string RowTextToPaint(void* pBook, int row) {
    if (row < 0 || row > 1) {
        return std::string();
    }
    if (row == g_st.editRow) {
        return ModuleOwnsRowText(row) ? ReadEditText(pBook) : std::string();
    }
    return g_st.rowText[row];
}

// K3.1: by TEXT, for both rows, hosting or not. `RowText` is the live control for the hosting row
// and the stash for the other, so this is the row's text in every state.
int BoxVariantForRow(void* pBook, int row) {
    if (row < 0 || row > 1) {
        return kBoxVariant_Placeholder;
    }
    return RowText(pBook, row).empty() ? kBoxVariant_Placeholder : kBoxVariant_Empty;
}

// True while the stock edit is physically on this row, on screen, and holding glyphs that only IT
// can put back. That is the exact state in which an out-of-band opaque box blit destroys visible
// text with nothing scheduled to redraw it.
bool EditOwnsLiveGlyphs(void* pBook, int row) {
    if (row != g_st.editRow || ModuleOwnsRowText(row)) {
        return false; // not hosting, or the module draws this row's text itself anyway
    }
    void* pEdit = StockEdit(pBook);
    if (!pEdit || SafeReadInt(pEdit, kOff_Ctrl_Shown) == 0) {
        return false; // parked / hidden -> there are no glyphs on screen to protect
    }
    return !ReadEditText(pBook).empty();
}

// Blit one field box plus -- when this row's surface makes the module the text's owner -- its text
// and its caret. `dstX/dstY` are already in the target canvas's own space.
// `why` is kPaintWhy_DrawPass inside a draw hook and kPaintWhy_OutOfBand everywhere else; see the
// ordering block above for why that distinction decides whether the interior may be touched.
void PaintRow(IWzCanvasPtr& target, void* pBook, int row, int dstX, int dstY, int why) {
    if (!target) {
        return;
    }
    const bool hosting = (row == g_st.editRow);
    const int surface = kRowSurface[row];
    // THE ORDERING GUARD (see the block above kPaintWhy_DrawPass). Out of the draw pass, the window
    // canvas is a retained buffer the edit has already drawn into and will not redraw on its own.
    if (why == kPaintWhy_OutOfBand && surface == kSurface_Window && EditOwnsLiveGlyphs(pBook, row)) {
        g_st.stripRepaintPending = true;
        return;
    }
    if (surface == kSurface_Window) {
        g_st.stripRepaintPending = false;
    }
    const std::string text = RowTextToPaint(pBook, row);
    const int variant = BoxVariantForRow(pBook, row);
    g_st.rowPainted[row] = text;
    g_st.paintedHostRow = g_st.editRow;
    const bool haveArt = ResolveBox(row, variant);
    if (haveArt) {
        try {
            target->raw_Copy(dstX, dstY, g_box[row][variant].canvas, vtEmpty);
        } catch (...) {
            return;
        }
    }

    // K3.2/K3.3: the module's glyphs must land EXACTLY where the control's would, because on row 1
    // they now share the field with the caret this function also draws. The client's own numbers:
    //   x = get_rx(ctrl+0x18) + [ctrl+0x38] - [ctrl+0x60]     (0x004CAFDD..0x004CAFF0)
    //   y = get_ry(ctrl+0x18) + [ctrl+0x3C]                   (0x004CB71B..0x004CB72E)
    // and the book's CREATEPARAM (0x004C8D5F) ships insetX 0 and insetY **-2**, so the old
    // `if (iy < 0) iy = 1` clamp was throwing away a real, negative inset and drawing the stash
    // three pixels low. Negatives are accepted now; only absurd values fall back.
    // Read UNCONDITIONALLY (round 11): the caret needs the same base x on an empty row too.
    void* pEdit = StockEdit(pBook);
    int ix = pEdit ? SafeReadInt(pEdit, kOff_Ctrl_TextInsetX) : 0;
    int iy = pEdit ? SafeReadInt(pEdit, kOff_Ctrl_TextInsetY) : -2;
    int hscroll = 0;
    if (ix < -8 || ix > 16) {
        ix = 0;
    }
    if (iy < -8 || iy > 16) {
        iy = -2;
    }
    // The h-scroll only exists for the row the control is actually on.
    if (hosting) {
        hscroll = SafeReadInt(pEdit, kOff_Ctrl_HScroll);
        if (hscroll < 0 || hscroll > 4096) {
            hscroll = 0;
        }
    }
    IWzFont* pFont = EnsureRowFont();
    const int textX = dstX + (kFieldX - kBoxX) + ix - hscroll;
    const int textY = dstY + (kFieldY[row] - kBoxY[row]) + iy;
    if (!text.empty() && pFont) {
        try {
            unsigned drawn = 0;
            Ztl_bstr_t s(text.c_str());
            // Box art is blitted at (dstX,dstY) == window (48, rowBoxY); the control draws at
            // window (49 + insetX - hscroll, fieldY + insetY).
            target->raw_DrawText(textX, textY, s, pFont, vtEmpty, vtEmpty, &drawn);
        } catch (...) {
        }
    }

    // ---------------------------------------------------------------------------------------------
    // THE CARET, for a row whose box covers the control's own output.
    //
    // Row 0 keeps the engine's caret: its box lands on the window canvas UNDER the child pass, so
    // `CCaret` composites over it exactly as it always has -- and as of round 12 it lands on the
    // right ROW, because `SyncEditCaret` re-bakes `caret+0x00/+0x04` after every move (see the
    // derivation above `kAddr_CCtrlEdit_UpdateCaret`). Row 1's box is on the left-page LAYER, which
    // composites ABOVE the window canvas (6.K-K0), so the engine's caret is behind our box there and
    // the field reads as focus-less however hard the player clicks it. The module therefore draws
    // that row's caret itself, from the control's own state -- not from "end of string":
    //   index    [ctrl+0x48]                      (0x004CB792 End / 0x004CB797 Home / arrows)
    //   x        textX + width(text[0..index)) + ([ctrl+0x40] - [ctrl+0x38])
    //                                            (0x004CA598 bakes the caret's x base off +0x40
    //                                             while 0x004CAFE7 draws the glyphs off +0x38; the
    //                                             book ships both as 0, so the term is normally 0
    //                                             and exists only so the two carets cannot diverge)
    //   y        CaretInsetY()                    (the GLYPH inset, not stock's [ctrl+0x44] -- see
    //                                             CaretInsetY for why that is K3.3's "few px")
    //   h        [ctrl+0x7C] == [caret+0x10]      (the bar CCaret builds, 0x004C8E30 / 0x004C8F0F)
    //   blink    ((tick - [caret+8]) / 300) & 1   (CCaret::SetVisible, 0x004C932A)
    //   alive    shown && focused                 (0x004CA22D, the client's own two conditions)
    // Reading the engine's own phase anchor keeps the two carets in lockstep, so nothing on screen
    // can beat out of time with a caret drawn elsewhere in the client.
    // ---------------------------------------------------------------------------------------------
    bool caretDrawn = false;
    int caretX = -1;
    int caretPhase = -1;
    int caretIdx = -1;
    if (hosting && ModuleOwnsRowText(row) && pEdit && SafeReadInt(pEdit, kOff_Ctrl_Shown) != 0
            && FocusOwner() == reinterpret_cast<char*>(pEdit) + 4) {
        caretIdx = SafeReadInt(pEdit, kOff_Ctrl_Caret);
        const int len = static_cast<int>(text.size());
        if (caretIdx < 0 || caretIdx > len) {
            caretIdx = len; // a caret index the control has not caught up with yet
        }
        int advance = 0;
        if (caretIdx > 0 && pFont) {
            try {
                advance = static_cast<int>(pFont->CalcTextWidth(text.substr(0, caretIdx).c_str()));
            } catch (...) {
                advance = 0;
            }
            if (advance < 0 || advance > kFieldW * 4) {
                advance = 0;
            }
        }
        // Height: the bar CCaret actually built (`[caret+0x10]`, copied from `[ctrl+0x7C]` at
        // 0x004CA572) -- read from the caret itself when it is alive so the two agree by construction.
        int ch = SafeReadInt(pEdit, kOff_Ctrl_Caret_Obj + kOff_Caret_BarH);
        if (ch < 4 || ch > kBoxH) {
            ch = SafeReadInt(pEdit, kOff_Ctrl_LineH);
        }
        if (ch < 4 || ch > kBoxH) {
            ch = static_cast<int>(kRowFontHeight);
        }
        // K3.3: the SAME y offset SyncEditCaret bakes into the engine caret, so row 0's engine bar
        // and row 1's module bar sit at identical heights within their fields.
        const int cy = CaretInsetY(pEdit);
        // ...and the same x base. `ix` above is the GLYPH inset ([ctrl+0x38]); the caret's is
        // [ctrl+0x40] (0x004CA598), so the difference is what separates the two on this axis.
        int cxAdj = SafeReadInt(pEdit, kOff_Ctrl_CaretBaseX) - ix;
        if (cxAdj < -8 || cxAdj > 16) {
            cxAdj = 0;
        }
        const DWORD anchor = static_cast<DWORD>(
                SafeReadInt(pEdit, kOff_Ctrl_Caret_Obj + kOff_Caret_PhaseTick));
        const DWORD since = GetTickCount() - anchor;
        caretPhase = static_cast<int>((since / kCaretBlinkHalfMs) & 1u);
        caretX = textX + advance + cxAdj;
        // Never let it wander outside the field, whatever the control reports.
        const int leftLimit = dstX + (kFieldX - kBoxX);
        if (caretX < leftLimit) {
            caretX = leftLimit;
        }
        if (caretX > leftLimit + kFieldW - kCaretW) {
            caretX = leftLimit + kFieldW - kCaretW;
        }
        if (caretPhase == 0) {
            FillRect(target, caretX, dstY + (kFieldY[row] - kBoxY[row]) + cy, kCaretW, ch,
                    kCaretColor);
            caretDrawn = true;
        }
    }

}

// ITEM row -- WINDOW canvas, run right after CWnd::Draw re-blits `backgrnd`, so it lands ON the
// parchment and UNDER the child controls the same pass draws next. Same frame, so it cannot flicker.
void PaintItemRowBox(void* pBook, int why) {
    if (!OnTierScreen(pBook)) {
        return; // I1.2: home OR unknown -> the item row does not exist on this screen
    }
    IWzCanvasPtr wnd = WindowCanvas(pBook);
    if (!wnd) {
        return;
    }
    // CWnd::Draw does not assume (0,0): it blits `backgrnd` at [this+0x40],[this+0x44].
    const int dx = SafeReadInt(pBook, kOff_BgDstLeft);
    const int dy = SafeReadInt(pBook, kOff_BgDstTop);
    if (dx < -1024 || dx > 1024 || dy < -1024 || dy > 1024) {
        // Was a silent guard until round 11. It logs and re-arms like every other one in this file.
        return;
    }
    PaintRow(wnd, pBook, kRowItem, dx + kBoxX, dy + kBoxY[kRowItem], why);
}

// MOB row -- LEFT PAGE canvas. That layer sits at window (40,25) and composites OVER the window
// canvas (its `cardSlot` art is opaque across x 49..169), so window (48,29) is its local (8,4).
void PaintMobRowBox(void* pBook, int why) {
    if (!OnTierScreen(pBook)) {
        return; // I1.2, same split as the item row
    }
    IWzCanvasPtr page = LayerCanvas(pBook, kOff_LeftPageLayer);
    if (!page) {
        return;
    }
    PaintRow(page, pBook, kRowMob, kBoxX - kLeftPageOriginX, kBoxY[kRowMob] - kLeftPageOriginY,
why);
}

// =================================================================================================
// H1.10 -- "Search cards per level", HOME SCREEN ONLY
//
// The home screen (left tab 9) hides every stock control -- RefreshCtrls' whole first half is
// `SetShow(leftTab != 9)` -- so this control is drawn and hit-tested entirely here. It reuses the
// module's proven surfaces rather than inventing one:
//
//   * the BOX + magnifier go on the WINDOW canvas from the CWnd::Draw hook, at the item row's
//     (48,9)/(175,9). Round 5's bug report ("the item field shows and FLICKERS on the initial
//     screen") is the proof that a window-canvas blit at that y REACHES THE SCREEN on the cover.
//   * the unrolled DROPDOWN goes on the LEFT PAGE canvas from the Redraw(0) hook. Everything from
//     y >= 25 is covered by that layer -- which is exactly why the mob row is painted there -- and
//     Redraw(0) demonstrably runs on the cover as well: it reads `leftTab == 9` at its own head
//     (0x00863E1A) and paints the cover art into that same canvas.
//
// Both are pure canvas work, no engine controls, so there is nothing to create, show, hide or leak.
// =================================================================================================
CachedSprite g_magnifier;
CachedSprite g_magnifierOver; // K2.3 -- the hover frame the engine gives the real CCtrlButtons

bool ResolveMagnifier() {
    if (g_magnifier.canvas) {
        return true;
    }
    if (g_magnifier.gen == g_bookGen) {
        return false;
    }
    // ResolveSprite stamps the generation itself, so try the second spelling only when the first
    // one has already claimed this generation -- otherwise the fallback would never be reached.
    for (int i = 0; i < 2; ++i) {
        g_magnifier.gen = 0;
        if (ResolveSprite(g_magnifier, kUOL_MagnifierArt[i], 8, 64, 8, 40, "magnifier")) {
            return true;
        }
    }
    g_magnifier.gen = g_bookGen;
    return false;
}

// K2.3 -- same probe, `mouseOver` instead of `normal`. A client whose WZ has no mouseOver state is
// not a failure: the caller falls back to `normal`, i.e. to exactly today's flat look, and the log
// says which node was missing.
bool ResolveMagnifierOver() {
    if (g_magnifierOver.canvas) {
        return true;
    }
    if (g_magnifierOver.gen == g_bookGen) {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        g_magnifierOver.gen = 0;
        if (ResolveSprite(g_magnifierOver, kUOL_MagnifierOverArt[i], 8, 64, 8, 40,
                    "magnifier/over")) {
            return true;
        }
    }
    g_magnifierOver.gen = g_bookGen;
    return false;
}

// No silent guards anywhere in this module (spec 6.G G2 lists them as a round-5 failure class):
// every one of the small drawing helpers below reports through the shared, rate-limited paint slot,
// so a canvas that refuses to draw shows up in the log as a refusal rather than as an absence.
void FillRect(IWzCanvasPtr& c, int x, int y, int w, int h, unsigned color) {
    if (!c || w <= 0 || h <= 0) {
        return;
    }
    try {
        c->raw_DrawRectangle(x, y, static_cast<unsigned>(w), static_cast<unsigned>(h), color);
    } catch (...) {
    }
}

void FrameRect(IWzCanvasPtr& c, int x, int y, int w, int h, unsigned color) {
    FillRect(c, x, y, w, 1, color);
    FillRect(c, x, y + h - 1, w, 1, color);
    FillRect(c, x, y, 1, h, color);
    FillRect(c, x + w - 1, y, 1, h, color);
}

void DrawTextAt(IWzCanvasPtr& c, int x, int y, const char* text, IWzFont* pFont) {
    if (!c || !pFont || !text || !*text) {
        return;
    }
    try {
        unsigned drawn = 0;
        Ztl_bstr_t s(text);
        c->raw_DrawText(x, y, s, pFont, vtEmpty, vtEmpty, &drawn);
    } catch (...) {
    }
}

// A downward triangle, built out of 1 px rows so it needs no polygon support.
void DrawDownArrow(IWzCanvasPtr& c, int cx, int cy, unsigned color) {
    for (int i = 0; i < 4; ++i) {
        const int w = 7 - i * 2;
        FillRect(c, cx - w / 2, cy + i, w, 1, color);
    }
}

const char* LevelCaption(char* buf, size_t cap) {
    if (g_st.levelSel < 0) {
        return "Search cards per level";
    }
    _snprintf_s(buf, cap, _TRUNCATE, "level %d", g_st.levelSel);
    return buf;
}

// The placeholder is 22 characters and the box interior is ~100 px once the arrow cell is reserved,
// so it can overflow. `IWzFont::CalcTextWidth` is the same measurement `worldMapInfo.cpp` uses for
// its own fitting; a font that refuses to measure degrades to drawing the full string rather than
// to drawing nothing.
std::string FitCaption(IWzFont* pFont, const std::string& text, int maxWidth) {
    if (!pFont || text.empty() || maxWidth <= 0) {
        return text;
    }
    int full = 0;
    try {
        full = static_cast<int>(pFont->CalcTextWidth(text.c_str()));
    } catch (...) {
        return text;
    }
    if (full <= 0 || full <= maxWidth) {
        return text;
    }
    for (size_t len = text.size(); len > 1; --len) {
        const std::string candidate = text.substr(0, len - 1) + "..";
        int w = 0;
        try {
            w = static_cast<int>(pFont->CalcTextWidth(candidate.c_str()));
        } catch (...) {
            return text;
        }
        if (w <= maxWidth) {
            return candidate;
        }
    }
    return text;
}

// The closed control: box, caption, arrow, magnifier. WINDOW canvas.
void PaintLevelControl(void* pBook) {
    if (!OnHomeScreen(pBook)) {
        return; // I1.2: tier OR unknown -> the level control does not exist on this screen
    }
    IWzCanvasPtr wnd = WindowCanvas(pBook);
    if (!wnd) {
        return;
    }
    const int dx = SafeReadInt(pBook, kOff_BgDstLeft);
    const int dy = SafeReadInt(pBook, kOff_BgDstTop);
    if (dx < -1024 || dx > 1024 || dy < -1024 || dy > 1024) {
        // Round 11: this was the OTHER silent guard, and it was also the storm's fuel -- bailing
        // here leaves the "what I painted" trio untouched, so EnsureLevelBoxFresh saw a difference
        // it could never close. It logs now, and the freshness check has its own attempt guard.
        return;
    }
    const int bx = dx + kLvlBoxX;
    const int by = dy + kLvlBoxY;
    // K2.2 -- record what this pass is about to put on the window canvas. That surface has NO dirty
    // flag (only CWnd::Draw writes it, and only on a transition), so `EnsureLevelBoxFresh` compares
    // these three against the live state and repaints on the spot when the player picks a level.
    // Recorded here, after the canvas is known to be usable, so a skipped pass retries next frame.
    g_st.lvlPaintedSel = g_st.levelSel;
    g_st.lvlPaintedOpen = g_st.levelOpen ? 1 : 0;
    g_st.lvlPaintedHover = g_st.levelMagHover ? 1 : 0;

    // The box: the baked empty-box art when the WZ has it (identical look to the two search rows),
    // otherwise a hand-drawn white field with a grey frame.
    const bool baked = ResolveBox(kRowItem, 1);
    if (baked) {
        try {
            wnd->raw_Copy(bx, by, g_box[kRowItem][1].canvas, vtEmpty);
        } catch (...) {
        }
    } else {
        FillRect(wnd, bx, by, kLvlBoxW, kLvlBoxH, kLvlFillColor);
        FrameRect(wnd, bx, by, kLvlBoxW, kLvlBoxH, kLvlBorderColor);
    }

    IWzFont* pFont = EnsureFont();
    char buf[32];
    const char* caption = LevelCaption(buf, sizeof(buf));
    // Grey while it is still a placeholder, black once a level has been chosen -- the same reading
    // the two baked search boxes give. A missing grey font degrades to black, never to nothing.
    IWzFont* pCaptionFont = (g_st.levelSel < 0) ? EnsureHintFont() : pFont;
    if (!pCaptionFont) {
        pCaptionFont = pFont;
    }
    const std::string fitted =
            FitCaption(pCaptionFont, caption, kLvlBoxW - 6 - kLvlArrowW);
    DrawTextAt(wnd, bx + 3, by + 3, fitted.c_str(), pCaptionFont);
    DrawDownArrow(wnd, bx + kLvlBoxW - kLvlArrowW / 2 - 3, by + 7,
            g_st.levelOpen ? kLvlTextColor : kLvlHintColor);

    // I1.2 -- the level control draws its OWN magnifier, ALWAYS.
    //
    // Round 7 reused `BtSearch2` (nId 0x7DA) for both screens because it already sits at (175,9)
    // and its click already reaches OnButtonClicked. The ownership rule forbids exactly that: "the
    // two must never share a control, a rect, or a paint pass state". So BtSearch2 is the item
    // row's magnifier and nothing else, and the tick PARKS it off-screen whenever the book is not
    // on a tier screen -- necessary, not cosmetic, because `CWnd::HitTest` (0x009E01E7) walks the
    // child list and calls `CCtrlWnd::IsHit` (0x004DFECE, a bare PtInRect against {0,0,w,h}) and
    // consults NO shown flag, so a merely hidden BtSearch2 would still swallow this click.
    //
    // K2.3 -- and it draws its own HOVER state, because a module-drawn magnifier gets none from the
    // engine. `mouseOver/0` while the cursor is inside its rect, `normal/0` otherwise -- the same
    // two states the real `CCtrlButton`s at 0x7D0 / 0x7DA are skinned from.
    const bool wantOver = g_st.levelMagHover && ResolveMagnifierOver();
    const bool haveMag = wantOver || ResolveMagnifier();
    if (haveMag) {
        try {
            wnd->raw_Copy(dx + kLvlBtnX, dy + kLvlBtnY,
                    wantOver ? g_magnifierOver.canvas : g_magnifier.canvas, vtEmpty);
        } catch (...) {
        }
    } else {
        FillRect(wnd, dx + kLvlBtnX, dy + kLvlBtnY, kLvlBtnW, kLvlBtnH, kLvlFillColor);
        FrameRect(wnd, dx + kLvlBtnX, dy + kLvlBtnY, kLvlBtnW, kLvlBtnH, kLvlBorderColor);
        DrawTextAt(wnd, dx + kLvlBtnX + 10, dy + kLvlBtnY + 3, "Go", pFont);
    }

}

// Forward: the window-canvas pass, which is what actually repaints the level box.
void PaintItemRowGuarded(void* pBook, int why);

// -------------------------------------------------------------------------------------------------
// K2.2 -- THE LEVEL BOX REPAINTS THE INSTANT `levelSel` CHANGES.
//
// The box lives on the WINDOW canvas, and that surface is written by exactly one thing --
// `CWnd::Draw`, through primary slot 11 -- which the client only runs on user-driven transitions.
// The shipped log is unambiguous about the consequence: `PAINT level box` appears at CREATE and at
// screen changes and NEVER after a `LEVEL pick`, which is the reported "the choice only shows up
// after I leave and come back". `MarkAllDirty` cannot help, because the pane flags at book+0xB14
// drive `Redraw` (the two PAGE layers), not the window canvas.
//
// So the repaint is done here, on the spot, through the same guarded painter the draw hook uses.
// The box art is opaque 122x17 over the same rect, so re-blitting it fully replaces the old caption
// -- there is nothing to erase first. Panes are dirtied as well, because the DROPDOWN half of the
// control lives on the left page and follows `levelOpen`.
//
// ROUND 11 -- AND IT CANNOT STORM. The trio it compares against is written by `PaintLevelControl`,
// and that function has three early exits ahead of the write (wrong screen, no window canvas, an
// unplaced window). Any of them left the comparison permanently unequal, so this function repainted
// AND called `MarkAllDirty` on EVERY Update AND every tick -- and each of those dirty flags makes
// the client rebuild the left page through `Redraw(0)`, which allocates a fresh `Canvas` object at
// 0x00863E2E. Unbounded, per frame, for as long as the condition held. That is the same failure
// class the I1.5 note describes for the row-text detector, and it gets the same fix: remember the
// state that was ATTEMPTED, not only the state that was successfully painted, so one change of
// `levelSel` / `levelOpen` / `levelMagHover` buys exactly one attempt. A genuinely new state always
// retries, because the attempted trio moves with it.
// -------------------------------------------------------------------------------------------------
void EnsureLevelBoxFresh(void* pBook) {
    if (!pBook || !OnHomeScreen(pBook)) {
        return;
    }
    const int wantSel = g_st.levelSel;
    const int wantOpen = g_st.levelOpen ? 1 : 0;
    const int wantHover = g_st.levelMagHover ? 1 : 0;
    if (wantSel == g_st.lvlPaintedSel && wantOpen == g_st.lvlPaintedOpen
            && wantHover == g_st.lvlPaintedHover) {
        return; // already on screen
    }
    if (wantSel == g_st.lvlAttemptedSel && wantOpen == g_st.lvlAttemptedOpen
            && wantHover == g_st.lvlAttemptedHover) {
        // Already tried for exactly this state and the painter bailed (it logs why). Waiting for a
        // real change beats hammering MarkAllDirty every frame; the next draw pass paints it anyway.
        return;
    }
    g_st.lvlAttemptedSel = wantSel;
    g_st.lvlAttemptedOpen = wantOpen;
    g_st.lvlAttemptedHover = wantHover;
    PaintItemRowGuarded(pBook, kPaintWhy_OutOfBand); // -> PaintLevelControl, which records the trio
    MarkAllDirty(pBook);                             // the unrolled list is left-page work
}

// The unrolled list. LEFT PAGE canvas, so it survives the cover art the same pass just painted.
void PaintLevelDropdown(void* pBook) {
    if (!g_st.levelOpen || !OnHomeScreen(pBook)) {
        return; // I1.2, and the tick's assertion guarantees `levelOpen` is false off the home screen
    }
    IWzCanvasPtr page = LayerCanvas(pBook, kOff_LeftPageLayer);
    if (!page) {
        return;
    }
    // K2.1 -- flush under the box when the edit is parked out of the way, the round-8 safe y
    // otherwise. `LevelListY()` is the single answer both this pass and `HitLevelControl` read.
    const int x = kLvlListX - kLeftPageOriginX;
    const int y = LevelListY() - kLeftPageOriginY;
    FillRect(page, x, y, kLvlListW, kLvlListH, kLvlFillColor);
    IWzFont* pFont = EnsureFont();
    for (int i = 0; i < kLvlCount; ++i) {
        const int level = kLvlMinLevel + i;
        const int ry = y + i * kLvlRowH;
        if (level == g_st.levelSel) {
            FillRect(page, x + 1, ry, kLvlListW - 2, kLvlRowH, kLvlSelColor);
        }
        char row[24];
        _snprintf_s(row, _countof(row), _TRUNCATE, "level %d", level);
        DrawTextAt(page, x + 6, ry + 1, row, pFont);
    }
    FrameRect(page, x, y, kLvlListW, kLvlListH, kLvlBorderColor);
}

// The ITEM hits, on the RIGHT page, over the client's own slot rects, as plain icons with NO
// percentages. Each slot is re-based with the page's own IconBase art first so whatever the stock
// Dropping pass left behind cannot show through between our icons.
void PaintItemResults(void* pBook) {
    IWzCanvasPtr page = LayerCanvas(pBook, kOff_PageLayer);
    if (!page) {
        return;
    }
    ResolveSprite(g_iconBase, kUOL_IconBase, 8, 64, 8, 64, "iconBase");

    auto pItemInfo = CItemInfo::GetInstance();
    const int first = g_st.itemPage * kItemSlots;
    int drawn = 0;
    for (int slot = 0; slot < kItemSlots; ++slot) {
        RECT rc;
        if (!ReadRect(pBook, kOff_DropRects, slot, rc)) {
            continue;
        }
        if (g_iconBase.canvas) {
            page->raw_Copy(rc.left, rc.top, g_iconBase.canvas, vtEmpty);
        }
        const int idx = first + slot;
        if (idx < 0 || idx >= static_cast<int>(g_st.itemResults.size())) {
            continue;
        }
        const int itemId = g_st.itemResults[static_cast<size_t>(idx)];
        if (itemId <= 0 || !pItemInfo) {
            continue;
        }
        // The stock Dropping pass anchors the icon at (rect.left, rect.bottom) -- 0x00865D8B.
        try {
            pItemInfo->DrawItemIconForSlot(page, itemId, rc.left, rc.bottom, 0, 0, 0, 0, 0, 0);
            ++drawn;
        } catch (...) {
        }
    }
}

// A COM / WZ throw must never leave the engine's draw loop, and a hard fault must not take the
// client down. G0.2: a tripped guard LOGS and the next frame tries again -- it never latches.
void PaintItemResultsInner(void* pBook) {
    try {
        PaintItemResults(pBook);
    } catch (...) {
    }
}

void PaintItemResultsGuarded(void* pBook) {
    __try {
        PaintItemResultsInner(pBook);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The window-canvas pass for the (48,9) strip. I1.2: the branch is written as an if/else on the
// two exhaustive, mutually exclusive predicates rather than as two independently-gated calls, so
// the "exactly one painter, and NEITHER when the tab is unreadable" rule is visible right here
// instead of being an emergent property of two separate guards.
void PaintItemRowInner(void* pBook, int why) {
    try {
        if (OnTierScreen(pBook)) {
            PaintItemRowBox(pBook, why);
        } else if (OnHomeScreen(pBook)) {
            PaintLevelControl(pBook);
        }
    } catch (...) {
    }
}

void PaintItemRowGuarded(void* pBook, int why) {
    __try {
        PaintItemRowInner(pBook, why);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ROUND 11 -- the ONE entry point every out-of-draw-pass window-strip repaint goes through, so the
// ordering guard cannot be bypassed by a future caller that forgets it. `PaintRow` refuses the
// interior when the edit is hosting that row with live text and raises `stripRepaintPending`; the
// CWnd::Draw hook clears it on its next pass, which is where the box belongs.
void PaintWindowStripNow(void* pBook, const char* reason) {
    if (!pBook) {
        return;
    }
    PaintItemRowGuarded(pBook, kPaintWhy_OutOfBand);
}

// The left-page pass. Same exclusive split, plus -- on a tier screen only -- the I1.6 drop-%
// overlay on a droppers-from-item result grid, which is declared below and defined after the
// per-mob ppm table it reads.
void PaintDropperPercents(void* pBook);

void PaintMobRowInner(void* pBook, int why) {
    try {
        if (OnTierScreen(pBook)) {
            PaintMobRowBox(pBook, why);
            PaintDropperPercents(pBook);
        } else if (OnHomeScreen(pBook)) {
            PaintLevelDropdown(pBook);
        }
    } catch (...) {
    }
}

void PaintMobRowGuarded(void* pBook, int why) {
    __try {
        PaintMobRowInner(pBook, why);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// =================================================================================================
// G0.5 / G0.7 -- the chrome the item view borrows: the content tab strip and the right-page pagers
// =================================================================================================
void ParkTabStrip(void* pBook, const char* reason) {
    void* pTab = SafeReadPtr(pBook, kOff_RightTabCtrl);
    if (!pTab) {
        return;
    }
    if (g_st.tabParked && g_st.tabParkReason != reason) {
        g_st.tabParkReason = reason;
    }
    if (!g_st.tabParked) {
        g_st.tabParkReason = reason;
        int x = 0, y = 0;
        if (!CtrlReadPos(pTab, &x, &y)) {
            return;
        }
        if (x >= kTabSaneMaxX || x < -kTabSaneMaxX) {
            // Already parked and our bookkeeping lost it (see kTabHomeX). Recording this as home
            // would push the strip another kTabParkOffset out on every item view and it would
            // never come back. Take the stock home instead and say so out loud.
            x = kTabHomeX;
            y = kTabHomeY;
        }
        g_st.tabSaveX = x;
        g_st.tabSaveY = y;
        g_st.tabParked = true;
    }
    // Both levers, every tick, observe-then-correct: SetShow(0) is what RefreshCtrls itself uses,
    // and the off-screen RelMove is engine-native position which nothing can quietly ignore.
    const bool wroteShow = ShowCtrl(pTab, 0);
    int curX = 0, curY = 0;
    const bool haveXY = CtrlReadPos(pTab, &curX, &curY);
    const int wantX = g_st.tabSaveX + kTabParkOffset;
    if (!haveXY || curX != wantX) {
        const bool moved = CtrlMoveTo(pTab, wantX, g_st.tabSaveY);
        // Rate-limited: a strip that refuses to move would otherwise write ~20 lines a second, and
        // the one line that says so is worth more than a thousand that repeat it.
    }
}

void UnparkTabStrip(void* pBook) {
    void* pTab = SafeReadPtr(pBook, kOff_RightTabCtrl);
    if (!g_st.tabParked) {
        // SELF-HEAL. `tabParked` is per-book state and every reset path clears it -- including the
        // one that can fire while the book is still ALIVE (`adopt-new-book`; J-D deleted the other,
        // the `book-gone` idle timeout). That path cannot safely touch a control (the book may
        // equally well be gone), so a park can outlive its bookkeeping and the four content tabs would
        // stay 3000 px off-screen for the rest of that book's life -- exactly the "restored on
        // exit" hole in user point 6. This runs from the tick, i.e. only ever on a book that is
        // demonstrably still pumping Update, so reading and moving its strip is safe here.
        if (!pTab) {
            return;
        }
        int x = 0, y = 0;
        if (!CtrlReadPos(pTab, &x, &y) || (x < kTabSaneMaxX && x > -kTabSaneMaxX)) {
            return; // where it should be (or unreadable) -- nothing to do
        }
        const bool rmoved = CtrlMoveTo(pTab, kTabHomeX, kTabHomeY);
        const bool rshown = ShowCtrl(pTab, 1);
        return;
    }
    const bool moved = pTab ? CtrlMoveTo(pTab, g_st.tabSaveX, g_st.tabSaveY) : false;
    const bool shown = pTab ? ShowCtrl(pTab, 1) : false;
    const char* why = g_st.tabParkReason;
    g_st.tabParked = false;
    g_st.tabParkReason = "";
}

// -------------------------------------------------------------------------------------------------
// I1.2 -- the same park, applied to OUR magnifier.
//
// BtSearch2 belongs to the ITEM row and therefore to left tab 0..8 only. `SetShow(0)` is not enough
// to take it off the home screen: `CWnd::HitTest` (0x009E01E7) walks the child list and calls
// `CCtrlWnd::IsHit` (0x004DFECE) -- `PtInRect({0,0,w,h}, localPt)` and nothing else, no shown flag
// anywhere in either function -- so a hidden control at (175,9) still eats the click that belongs
// to the level magnifier. Position is the only lever the engine cannot ignore, so the button goes
// off-screen, exactly the way the content tab strip does.
//
// Self-healing on both sides: the park is re-asserted every tick from the READ-BACK position, and
// an un-parked-but-stray button is dragged home even when `mag2Parked` was lost (an
// `adopt-new-book` clears the flag while the control is still out there).
// -------------------------------------------------------------------------------------------------
void ParkSearchButton2(void* pBook, bool bPark) {
    void* pBtn = g_st.btSearch2;
    if (!pBtn || !IsOurCtrl(pBtn, pBook, kNId_BtSearch2)) {
        return;
    }
    const int homeX = kBtnX;
    const int homeY = kBtnY[kRowItem];
    const int parkX = homeX + kTabParkOffset;
    int curX = 0, curY = 0;
    const bool haveXY = CtrlReadPos(pBtn, &curX, &curY);
    const int wantX = bPark ? parkX : homeX;

    if (!haveXY || curX != wantX) {
        const bool moved = CtrlMoveTo(pBtn, wantX, homeY);
    }
    // Shown follows the screen split too, so the button is not merely invisible-but-hittable on one
    // screen and drawn-but-parked on the other.
    ShowCtrl(pBtn, bPark ? 0 : 1);
    g_st.mag2Parked = bPark;
}

// The two right-page pagers. RefreshCtrls SHOWS them on every colour tab but DISABLES them off the
// MOB's list length, so while the item view owns the page they are driven here and handed straight
// back to the client's own RefreshCtrls on the way out.
void EnforceListArrows(void* pBook, bool bOwn) {
    void* pPrev = SafeReadPtr(pBook, kOff_ArrowListPrev);
    void* pNext = SafeReadPtr(pBook, kOff_ArrowListNext);
    if (!bOwn) {
        if (g_st.arrowsForced) {
            g_st.arrowsForced = false;
            RefreshCtrlsNow(pBook); // the client's own restore, once
        }
        return;
    }
    const int pages = ItemPageCount();
    const bool wantPrev = g_st.itemPage > 0;
    const bool wantNext = (g_st.itemPage + 1) < pages;
    if (!g_st.arrowsForced) {
        g_st.arrowsForced = true;
    }
    const bool a = ShowCtrl(pPrev, 1);
    const bool b = ShowCtrl(pNext, 1);
    const bool c = EnableCtrl(pPrev, wantPrev ? 1 : 0);
    const bool d = EnableCtrl(pNext, wantNext ? 1 : 0);
}

// Same for the LEFT-page pagers while a card result set owns the grid: RefreshCtrls enables them
// off the COLOUR TAB's own page count (0x00863D32 / 0x00863D68), which has nothing to do with how
// many pages of results we are showing.
void EnforceCardArrows(void* pBook, bool bOwn) {
    if (!bOwn) {
        return; // RefreshCtrls owns them again the moment the card view goes; nothing to undo
    }
    void* pPrev = SafeReadPtr(pBook, kOff_ArrowCardPrev);
    void* pNext = SafeReadPtr(pBook, kOff_ArrowCardNext);
    const int pages = CardPageCount();
    const bool a = ShowCtrl(pPrev, 1);
    const bool b = ShowCtrl(pNext, 1);
    const bool c = EnableCtrl(pPrev, g_st.cardPage > 0 ? 1 : 0);
    const bool d = EnableCtrl(pNext, (g_st.cardPage + 1) < pages ? 1 : 0);
}

// Park / unpark the CONTENT TAB INDEX. Draw reads it at 0x00865792 and dispatches with
// `sub eax,0 / dec / dec / dec`; anything outside 0..3 falls through to the tail at 0x00866808 --
// a state the client already handles, and one it can never be left in, because the parking is
// strictly paired inside a single Update by the RAII below.
bool ParkRightTabIndex(void* pBook, int& savedOut) {
    void* pTab = SafeReadPtr(pBook, kOff_RightTabCtrl);
    if (!pTab) {
        return false;
    }
    const int cur = SafeReadInt(pTab, kOff_TabSelected);
    if (cur < 0 || cur > 3) {
        // Not a silent skip: a strip STUCK on our own park value (4) would make Draw fall to its
        // tail for ever and the mob page would never render again, and the reader has to be able
        // to see that from the log rather than infer it from a blank page.
        return false;
    }
    if (!SafeWriteInt(pTab, kOff_TabSelected, kRightTabNone)) {
        return false;
    }
    savedOut = cur;
    return true;
}

void UnparkRightTabIndex(void* pBook, int saved) {
    void* pTab = SafeReadPtr(pBook, kOff_RightTabCtrl);
    if (pTab && SafeReadInt(pTab, kOff_TabSelected) == kRightTabNone) {
        SafeWriteInt(pTab, kOff_TabSelected, saved);
    }
}

// The view's own header. Draw's tail centres book+0xE24 (the mob name) on the right page; that is
// an ordinary refcounted ZXString<char> and Kentakae's ZXString is the same 4-byte handle over the
// same 0xC-byte header and the same allocator, so our caption is LENT to it for exactly the length
// of one Update and taken back afterwards. Nothing is freed on the client's behalf.
ZXString<char>* g_headerText = nullptr; // our caption (leaked on purpose -- see below)
ZXString<char>* g_headerHold = nullptr; // the book's own string, held while ours is installed
bool g_headerSwapped = false;
std::string g_headerBuilt;

// Deliberately never destroyed: a file-scope ZXString would run its destructor at DLL unload, after
// the engine's allocator may already be gone.
bool EnsureHeaderStrings() {
    try {
        if (!g_headerText) {
            g_headerText = new ZXString<char>();
        }
        if (!g_headerHold) {
            g_headerHold = new ZXString<char>();
        }
    } catch (...) {
    }
    return g_headerText != nullptr && g_headerHold != nullptr;
}

void BuildHeaderCaption() {
    if (!EnsureHeaderStrings()) {
        return;
    }
    const std::string q = g_st.itemQuery.size() > 18 ? g_st.itemQuery.substr(0, 18) : g_st.itemQuery;
    const int hits = static_cast<int>(g_st.itemResults.size());
    const int pages = ItemPageCount();
    char page[24];
    if (pages > 1) {
        _snprintf_s(page, _countof(page), _TRUNCATE, "  %d/%d", g_st.itemPage + 1, pages);
    } else {
        page[0] = '\0';
    }
    char buf[96];
    if (q.empty()) {
        _snprintf_s(buf, _countof(buf), _TRUNCATE, "Item search (%d)%s", hits, page);
    } else {
        _snprintf_s(buf, _countof(buf), _TRUNCATE, "Items: %s (%d)%s", q.c_str(), hits, page);
    }
    if (g_headerBuilt == buf) {
        return;
    }
    try {
        *g_headerText = buf;
        g_headerBuilt = buf;
    } catch (...) {
        g_headerBuilt.clear();
    }
}

// Refuse the swap while the book's own string sits in ZXString's transient nRef == -1 state (what
// GetBuffer leaves behind mid-Format): assigning out of it would deep-copy and then free the
// original buffer. Draw never runs against a string in that state, so this only ever declines a
// swap that would have been fine -- which is the right way round.
bool HeaderStringSwappable(void* pBook) {
    char* p = static_cast<char*>(SafeReadPtr(pBook, kOff_MobName));
    if (!p) {
        return true; // empty ZXString -- no buffer to protect
    }
    return SafeReadInt(p - 12, 0) > 0; // _ZXStringData::nRef, 0xC bytes before the characters
}

void SwapHeaderIn(void* pBook) {
    if (g_headerSwapped || !pBook || !EnsureHeaderStrings() || g_headerBuilt.empty()) {
        return;
    }
    if (!HeaderStringSwappable(pBook)) {
        // Declining is correct (see HeaderStringSwappable), but a header that NEVER appears while
        // the item view is up reads in game as "the view did not take", so say which of the two it
        // is instead of leaving the reader to guess.
        return;
    }
    ZXString<char>* pMember =
            reinterpret_cast<ZXString<char>*>(reinterpret_cast<char*>(pBook) + kOff_MobName);
    try {
        *g_headerHold = *pMember; // AddRef the book's, so our assignment cannot free it
        *pMember = *g_headerText;
        g_headerSwapped = true;
    } catch (...) {
        g_headerSwapped = false;
    }
}

void SwapHeaderOut(void* pBook) {
    if (!g_headerSwapped) {
        return;
    }
    g_headerSwapped = false;
    if (!pBook || !g_headerHold) {
        return;
    }
    ZXString<char>* pMember =
            reinterpret_cast<ZXString<char>*>(reinterpret_cast<char*>(pBook) + kOff_MobName);
    try {
        *pMember = *g_headerHold;
        *g_headerHold = ZXString<char>(); // drop our reference to the book's string
    } catch (...) {
    }
}

// RAII so a throw out of the client's own Update can never leave the book wearing our header or a
// parked tab index.
struct ItemViewChrome {
    void* pBook;
    int savedTab;
    bool parked;

    explicit ItemViewChrome(void* p) : pBook(p), savedTab(0), parked(false) {
        BuildHeaderCaption();
        parked = ParkRightTabIndex(pBook, savedTab);
        SwapHeaderIn(pBook);
    }
    ~ItemViewChrome() {
        SwapHeaderOut(pBook);
        if (parked) {
            UnparkRightTabIndex(pBook, savedTab);
        }
    }
    ItemViewChrome(const ItemViewChrome&) = delete;
    ItemViewChrome& operator=(const ItemViewChrome&) = delete;
};

// =================================================================================================
// H1.6/8 + H1.9 -- tooltips
//
// The invoker itself was already byte-faithful to `CUIMonsterBook::OnMouseMove`: `CUIToolTip` at
// book+0x5C0 (`lea ecx,[esi+0x5bc]` with esi == book+4, 0x008624E9), filled through 0x008F5B20 with
// (GetAbsLeft()+rx, GetAbsTop()+ry+0x14, itemId, blob, 0,0,0,0) -- verified push by push at
// 0x008624C3..0x008624F0, and 0x008F5B20 really does end in `ret 0x20`, so the eight-argument
// __thiscall declaration is right. `blob` is the same stack `ItemSlotTemp` the stock path builds
// (0x00483EED zeroes +0..+0x14 / +0x28 and seeds +0x18/+0x1C from two globals; 0x00483F18 is a
// no-op while +0x28 is 0), so a 0x60-byte local run through the real ctor is exactly what the
// client hands it.
//
// So round 7 changes the three things that could still differ from stock, and MEASURES the rest:
//   1. GetAbsLeft/GetAbsTop now go through IUIMsgHandler slots 11/12 the way the client calls them
//      (`call [eax+0x2c]` / `[eax+0x30]`) instead of two hardcoded addresses.
//   2. A ClearToolTip immediately BEFORE the Set. `CUIToolTip::SetItemToolTip` ends in a
//      "same content, same position -> do nothing" fast path (0x008E8267..0x008E829B compares the
//      cached title and the cached x/y), and a stale entry left over from the mob page satisfies
//      it; clearing first cannot be wrong and removes that whole class of no-op.
//   3. `m_nHeight` / `m_nWidth` (tooltip +0x08 / +0x0C, see wvs/tooltip.h) are read back and
//      LOGGED. A tooltip that built nothing reports height 0 -- that single number separates
//      "our call never ran" from "it ran and produced an empty panel", which is exactly the
//      question this round could not answer from the disassembly alone.
//   4. If the book's own tooltip reports height 0, the SAME call is retried on a PRIVATE CUIToolTip
//      of ours -- constructed with 0x008E49B5 in a static buffer, which is precisely how the
//      shipped, working `features/world/worldMapInfo.cpp` drives its own tooltip. If book+0x5C0 is
//      not usable at hover time (the spec's other hypothesis) this is what makes the tooltip appear
//      anyway, and the log says which one won.
// SEH-only: nothing in here needs C++ unwinding.
// =================================================================================================
alignas(8) unsigned char g_privateTip[0x600]; // CUIToolTip is < 0x440; worldMapInfo reserves 0x600
bool g_privateTipReady = false;
bool g_privateTipInUse = false;

void* PrivateTip() {
    if (!g_privateTipReady) {
        __try {
            memset(g_privateTip, 0, sizeof(g_privateTip));
            reinterpret_cast<void(__thiscall*)(void*)>(kAddr_ToolTip_Ctor)(g_privateTip);
            g_privateTipReady = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }
    return g_privateTip;
}

void ClearTip(void* pTip) {
    if (!pTip) {
        return;
    }
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(kAddr_ToolTip_Clear)(pTip);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void ClearPrivateTip() {
    if (g_privateTipReady && g_privateTipInUse) {
        g_privateTipInUse = false;
        ClearTip(g_privateTip);
    }
}

// GetAbsLeft / GetAbsTop through the sub-object's own vtable, like the client.
bool ReadAbsOrigin(void* pSubObj, int* outL, int* outT) {
    __try {
        void** vt = *reinterpret_cast<void***>(pSubObj);
        if (!vt || !vt[kVTIdx_GetAbsLeft] || !vt[kVTIdx_GetAbsTop]) {
            return false;
        }
        *outL = reinterpret_cast<int(__thiscall*)(void*)>(vt[kVTIdx_GetAbsLeft])(pSubObj);
        *outT = reinterpret_cast<int(__thiscall*)(void*)>(vt[kVTIdx_GetAbsTop])(pSubObj);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The tooltip's item slot. ShowItemToolTip's third argument is a GW_ItemSlotBase*, not an item id
// (0x008F5B9A: `esi=[ebp+0x10]; lea ebx,[esi+0xC]; call TSecType::GetData`). The reference module
// passed the id there, so the engine dereferenced (and every 55th call re-encoded) itemId+0xC.
// Built with CItemInfo::GetItemSlot and kept until the hovered item changes, since it allocates.
void* g_tipSlotRef[2] = { nullptr, nullptr }; // ZRef<GW_ItemSlotBase>: [1] is the object
int g_tipSlotItem = 0;

void ReleaseTipSlot() {
    if (g_tipSlotRef[1]) { // 0x00428A50 dereferences p unconditionally
        __try {
            reinterpret_cast<void(__thiscall*)(void*, int)>(kAddr_ZRefItemSlot_Release)(g_tipSlotRef, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    g_tipSlotRef[0] = g_tipSlotRef[1] = nullptr;
    g_tipSlotItem = 0;
}

void* TipSlotFor(int itemId) {
    if (itemId != g_tipSlotItem) {
        ReleaseTipSlot();
        void* pItemInfo = *reinterpret_cast<void**>(kAddr_CItemInfo_Instance);
        if (pItemInfo) {
            __try {
                reinterpret_cast<void*(__thiscall*)(void*, void*, long)>(kAddr_CItemInfo_GetItemSlot)(
                        pItemInfo, g_tipSlotRef, itemId);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_tipSlotRef[0] = g_tipSlotRef[1] = nullptr;
            }
        }
        g_tipSlotItem = itemId;
    }
    return g_tipSlotRef[1];
}

// Returns the height the tooltip ended up with (0 = it built nothing). Call shape as cashshopwnd.cpp
// (the CAdminShopDlg one): item slot, no ItemToolTipParam, zeros after.
int SetItemTipOn(void* pTip, int absX, int absY, int itemId) {
    void* pSlot = TipSlotFor(itemId);
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(kAddr_ToolTip_Clear)(pTip);
        if (!pSlot) {
            return 0; // unknown id: the caller falls back to the name-only tooltip
        }
        reinterpret_cast<void(__thiscall*)(void*, int, int, void*, void*, void*, int, int, int)>(
                kAddr_ToolTip_SetItem)(pTip, absX, absY, pSlot, nullptr, nullptr, 0, 0, 0);
        return *reinterpret_cast<const int*>(reinterpret_cast<const char*>(pTip) + kOff_Tip_Height);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Both defined below -- the item tooltip's I1.7 fallback needs them, and moving either up here
// would drag its own dependencies with it.
int SetTextTipOn(void* pTip, int absX, int absY, const char* title, const char* desc, int width);
std::string ItemName(int itemId);

void ShowItemToolTip(void* pSubObj, void* pBook, int rx, int ry, int itemId) {
    void* pBookTip = reinterpret_cast<char*>(pBook) + kOff_ToolTip;
    g_st.lastHoverTick = GetTickCount();
    if (itemId <= 0) {
        ClearTip(pBookTip);
        ClearPrivateTip();
        g_st.tipOurs = false;
        return;
    }
    int absL = 0, absT = 0;
    if (!ReadAbsOrigin(pSubObj, &absL, &absT)) {
        return;
    }
    const int absX = absL + rx;
    const int absY = absT + ry + 0x14;

    const int h = SetItemTipOn(pBookTip, absX, absY, itemId);
    g_st.tipOurs = true;
    if (h > 0) {
        ClearPrivateTip();
        return;
    }

    // The book's own tooltip produced nothing. Retry on the private one (worldMapInfo's pattern).
    void* pMine = PrivateTip();
    const int h2 = pMine ? SetItemTipOn(pMine, absX, absY, itemId) : -1;
    g_privateTipInUse = (h2 > 0);
    if (h2 > 0) {
        return;
    }

    // I1.7 -- THE GUARANTEED PATH.
    //
    // Two rounds of "the stock invoker is byte-faithful to CUIMonsterBook::OnMouseMove and the
    // tooltip still does not appear" have now been shipped, and both times the readback said the
    // panel built nothing (height 0). The invoker stays -- with its readbacks, because that number
    // is the only diagnostic anyone has -- but it is no longer the only route: when it produces
    // nothing, the item's NAME goes up through `SetToolTip_String2` (0x008E7150) instead, which is
    // the same call family the H1.9 card tooltip uses and the one `features/world/worldMapInfo.cpp`
    // has shipped working for months. A name-only panel is worth incomparably more than the nothing
    // the player gets today.
    const std::string name = ItemName(itemId);
    int h3 = -1;
    if (!name.empty()) {
        int width = static_cast<int>(name.size()) * 7 + 24;
        if (width < 80) {
            width = 80;
        }
        if (width > 260) {
            width = 260;
        }
        h3 = SetTextTipOn(pBookTip, absX, absY, name.c_str(), "", width);
        if (h3 <= 0) {
            void* pFallbackTip = PrivateTip();
            if (pFallbackTip) {
                h3 = SetTextTipOn(pFallbackTip, absX, absY, name.c_str(), "", width);
                g_privateTipInUse = (h3 > 0);
            }
        }
    }
}

// ---- H1.9: "«Mob Name» lv.N" over any card icon -------------------------------------------------
// Same object and the same screen-space anchor as the item tooltip, but built with
// CUIToolTip::SetToolTip_String2 (0x008E7150) -- the call `features/world/worldMapInfo.cpp` already
// ships and which is proven to render in game. Only ONE tooltip object is ever populated, so the
// stock Dropping tooltip and this one can never both be on screen.
int SetTextTipOn(void* pTip, int absX, int absY, const char* title, const char* desc, int width) {
    try {
        reinterpret_cast<void(__thiscall*)(void*)>(kAddr_ToolTip_Clear)(pTip);
        ZXString<char> sTitle(title);
        ZXString<char> sDesc(desc);
        reinterpret_cast<void(__thiscall*)(void*, int, int, ZXString<char>, ZXString<char>, int, int,
                int, int, int, int)>(kAddr_ToolTip_SetString2)(
                pTip, absX, absY, sTitle, sDesc, 0, 0, 0, width, 1, 0);
        return SafeReadInt(pTip, kOff_Tip_Height);
    } catch (...) {
        return -1;
    }
}

void ShowCardToolTip(void* pSubObj, void* pBook, int rx, int ry, const std::string& name, int level) {
    void* pBookTip = reinterpret_cast<char*>(pBook) + kOff_ToolTip;
    g_st.lastHoverTick = GetTickCount();
    int absL = 0, absT = 0;
    if (!ReadAbsOrigin(pSubObj, &absL, &absT)) {
        return;
    }
    char desc[48];
    if (level > 0) {
        _snprintf_s(desc, _countof(desc), _TRUNCATE, "lv.%d", level);
    } else {
        desc[0] = '\0';
    }
    // Compact: wide enough for the longer of the two lines at ~6 px per character, clamped so a
    // long mob name cannot produce a panel wider than the book itself.
    int width = static_cast<int>(name.size()) * 7 + 24;
    if (width < 70) {
        width = 70;
    }
    if (width > 260) {
        width = 260;
    }
    const int h = SetTextTipOn(pBookTip, absL + rx, absT + ry + 0x14, name.c_str(), desc, width);
    g_st.tipOurs = true;
}

// Drop whatever tooltip we own. Called whenever the hover leaves our territory, and on every reset.
void DropOurToolTip(void* pBook) {
    if (!g_st.tipOurs) {
        return;
    }
    g_st.tipOurs = false;
    g_st.hoverCardMob = 0;
    if (pBook) {
        ClearTip(reinterpret_cast<char*>(pBook) + kOff_ToolTip);
    }
    ClearPrivateTip();
}

// Which of OUR item slots the cursor is over, or -1. Same rects and the same (+240,+20) translation
// the stock hover uses (0x008623F2..0x0086241A), read out of the table the client itself built.
int HitItemSlot(void* pBook, int rx, int ry) {
    for (int slot = 0; slot < kItemSlots; ++slot) {
        RECT rc;
        if (!ReadRect(pBook, kOff_DropRects, slot, rc)) {
            continue;
        }
        if (PtIn(rx, ry, rc.left + kPageOriginX, rc.top + kPageOriginY, rc.right - rc.left,
                    rc.bottom - rc.top)) {
            return slot;
        }
    }
    return -1;
}

int HitCardSlot(void* pBook, int rx, int ry) {
    for (int slot = 0; slot < kGridSlots; ++slot) {
        RECT rc;
        if (!ReadRect(pBook, kOff_CardRects, slot, rc)) {
            continue;
        }
        if (PtIn(rx, ry, rc.left + kCardOriginX, rc.top + kCardOriginY, rc.right - rc.left,
                    rc.bottom - rc.top)) {
            return slot;
        }
    }
    return -1;
}

int ItemAtSlot(int slot) {
    if (slot < 0) {
        return 0;
    }
    const int idx = g_st.itemPage * kItemSlots + slot;
    if (idx < 0 || idx >= static_cast<int>(g_st.itemResults.size())) {
        return 0;
    }
    return g_st.itemResults[static_cast<size_t>(idx)];
}

// ONE card record, addressed exactly the way GetSlotCard addresses it (0x00867A5B, verified
// instruction by instruction: tab 9 rejected, page bounds-checked against the page array's
// `[eax-4]`, index against the entry array's, record at `[entries + index*8 + 4]`), but WITHOUT
// calling the original: GetSlotCard AddRefs what it hands back (`call 0x00685F37` at 0x00867AA9)
// and nothing here wants to own a reference. Reads only -- no WZ, no allocation, four derefs.
//
// This exists because the point-9 path (single click on a normal mob's Dropping icon) used to
// resolve the selected mob through this module's CARD INDEX, and that index is only ever built by
// a mob search or a droppers reply. On a book where the player has searched for nothing, it is
// empty, SelectedMobId returned 0, the item never resolved and the click silently fell through to
// the client -- i.e. the feature did nothing at all until you had used the search first.
int CardRecordMobAt(void* pBook, int tab, int page, int index) {
    if (!pBook || tab < 0 || tab >= kTabCount || page < 0 || index < 0) {
        return 0;
    }
    void* pPages = SafeReadPtr(pBook, kOff_TabPages + static_cast<uint32_t>(tab) * 4u);
    const int nPages = SafeArrayCount(pPages);
    if (nPages <= 0 || nPages > kMaxPagesPerTab || page >= nPages) {
        return 0;
    }
    void* pEntries = SafeIndexPtr(pPages, page);
    const int nEntries = SafeArrayCount(pEntries);
    if (nEntries <= 0 || nEntries > kMaxEntriesPerPage || index >= nEntries) {
        return 0;
    }
    void* pRec = SafeIndexPtr(pEntries, index * 2 + 1); // stride 8, record at +4
    if (!pRec) {
        return 0;
    }
    const int mobId = SafeReadInt(pRec, 4); // { +0 cardId, +4 mobId, +8 tab, +0xC ordinal }
    return mobId > 0 ? mobId : 0;
}

// =================================================================================================
// K5.1 -- ONE RESULT-GRID REMAP, SHARED BY EVERY PATH THAT ASKS WHAT IS IN A SLOT
//
// The GetSlotCard hook, the drop-% overlay, the hover tooltip and the selected-mob read used to
// carry four hand-written copies of `cardResults[page * 25 + slot]`, each with its own idea of where
// `page` comes from. That is exactly the class of divergence 6.K-K5.1 reports ("the hover path is
// resolving slot->mob through a route the dropper remap does not feed"), so there is now ONE
// resolver and one page rule, and every caller goes through them.
// =================================================================================================

// -> index into `g_cards`, or -1. Pure; no engine calls, no allocation.
//
// K7(b): the stride and the slot bound are `ResultSlotsPerPage()`, so in a DROPPER view slots
// 20..24 answer "no result here" -- the GetSlotCard hook then hands the client its own tab-9 "no
// card" record, the client draws an empty slot, and the label pass never has to place a label under
// a card whose bottom is at canvas y 249 on a 256 px surface.
int ResultCardIndexAt(int page, int slot) {
    const int per = ResultSlotsPerPage();
    if (!g_st.cardView || page < 0 || slot < 0 || slot >= per) {
        return -1;
    }
    const long long idx = static_cast<long long>(page) * per + slot;
    if (idx < 0 || idx >= static_cast<long long>(g_st.cardResults.size())) {
        return -1;
    }
    const int ci = g_st.cardResults[static_cast<size_t>(idx)];
    if (ci < 0 || ci >= static_cast<int>(g_cards.size())) {
        return -1;
    }
    return ci;
}

int ResultMobAt(int page, int slot) {
    const int ci = ResultCardIndexAt(page, slot);
    return (ci >= 0) ? g_cards[static_cast<size_t>(ci)].mobId : 0;
}

// The page the grid was ACTUALLY drawn from. `CUIMonsterBook::Draw` hands GetSlotCard the client's
// own `book+0x5B0` (0x00864914), so that is the authority -- but `SetSelectedCard` and the tab
// notify both rewrite it to a card's REAL page mid-click (0x0086798D / 0x00861FDD), and the Update
// hook only forces it back AFTER the original Update. So a `+0x5B0` that addresses nothing in the
// result set falls back to the module's own page rather than answering "no card".
int ResultGridPage(void* pBook) {
    const int book = SafeReadInt(pBook, kOff_CardPage);
    if (book >= 0 && ResultCardIndexAt(book, 0) >= 0) {
        return book;
    }
    if (ResultCardIndexAt(g_st.cardPage, 0) >= 0) {
        return g_st.cardPage;
    }
    return (book >= 0) ? book : g_st.cardPage;
}

// The mob whose page is on screen, resolved WITHOUT touching the refcounted card record: the book's
// own selected grid slot (+0x5B0 page, +0x5B8 slot) is read straight out of the tab-page arrays,
// or -- while a result view owns the grid -- through the same remap the GetSlotCard hook applies.
int SelectedMobId(void* pBook) {
    const int page = SafeReadInt(pBook, kOff_CardPage);
    const int slot = SafeReadInt(pBook, kOff_SelSlot);
    if (page < 0 || slot < 0 || slot >= kGridSlots) {
        return 0;
    }
    if (g_st.cardView) {
        // A result view owns the grid, so the book's own (tab, page, slot) is NOT what is on
        // screen -- the direct read below would answer with whatever card the untouched grid
        // would have held. Only the remap is a valid answer here; if the index is somehow gone,
        // the honest answer is "no mob", never a guess.
        if (g_cards.empty() || g_cardsForBook != pBook) {
            return 0;
        }
        return ResultMobAt(page, slot);
    }
    const int tab = LeftTabOrUnknown(pBook);
    if (tab < 0 || tab == kTabNoCard) {
        return 0;
    }
    return CardRecordMobAt(pBook, tab, page, slot);
}

// Is the Dropping list actually paging 16 per page? See kAddr_DropPagingImm. Read every time (it
// is one byte of already-resident .text) rather than cached, so a build order or a failed
// signature match can never leave this module quietly mapping slots onto the wrong ordinals.
bool DropPagingIsSixteen() {
    __try {
        return *reinterpret_cast<const unsigned char*>(kAddr_DropPagingImm)
                == static_cast<unsigned char>(kItemSlots);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// slot -> itemId on a NORMAL mob's Dropping tab. Same list the drop-% labels use:
// `String/MonsterBook.img/<mobId>/reward/<page*16 + slot>`.
int RewardItemAt(int mobId, int ordinal) {
    if (mobId <= 0 || ordinal < 0) {
        return 0;
    }
    try {
        wchar_t path[96];
        _snwprintf_s(path, _countof(path), _TRUNCATE, L"String/MonsterBook.img/%d/reward", mobId);
        Ztl_variant_t vList = get_object_or_empty(path);
        IWzPropertyPtr pList = vList.GetUnknown();
        if (!pList) {
            return 0;
        }
        wchar_t key[16];
        _snwprintf_s(key, _countof(key), _TRUNCATE, L"%d", ordinal);
        Ztl_variant_t v = get_item_or_empty(pList, key);
        const int id = get_int32(v, 0);
        return id > 0 ? id : 0;
    } catch (...) {
        return 0;
    }
}

// =================================================================================================
// I1.3 -- slot -> itemId, DECODED OUT OF THE FUSE the client itself drew the icon from
//
// The row walk is the client's own (stock hover, 0x0086246A..0x008624C0, `esi == book+4`):
//
//     pages   = book+0xE38                 ; ZArray, count at [pages-4]
//     page    = book+0x5B4                 ; the LIVE list page -- bounds-checked, as the client does
//     entries = pages[page]                ; ZArray, count at [entries-4]
//     record  = entries[slot*8 + 4]        ; stride 8, value at +4
//
// but the ID does NOT come from `record+0x1C`. Round 7 copied that from the stock hover; the list
// BUILDER (0x0086744E) and the icon DRAW (0x00865D68) both use `ZtlSecureFuse<int>::Get(record+0x0C)`
// instead. record+0x18 is a ZRef<GW_ItemSlotBase> (0x0086747B), so +0x1C is the item OBJECT pointer
// (what the stock hover hands ShowItemToolTip), never an id.
//
// The fuse is returned, and the caller keeps the
// WZ `reward` ordinal as the third opinion.
// =================================================================================================
struct DropRowIds {
    int fuse = 0;   // Get(record+0x0C)  -- what the icon was drawn from
    bool threw = false;
    bool haveRecord = false;
};

// The fuse's Get can THROW: a checksum mismatch runs `mov [ebp-0x10],5 / call 0x00A60BB7`
// (_CxxThrowException) at 0x004287DD. `catch (...)` is the right net for that; the __except one
// frame out is for a hard fault on a record the book has already freed.
int FuseGetInner(void* pFuse, bool* outThrew) {
    try {
        return reinterpret_cast<int(__thiscall*)(void*)>(kAddr_ZtlSecureFuse_Get)(pFuse);
    } catch (...) {
        if (outThrew) {
            *outThrew = true;
        }
        return 0;
    }
}

int FuseGetGuarded(void* pFuse, bool* outThrew) {
    if (!pFuse) {
        return 0;
    }
    __try {
        return FuseGetInner(pFuse, outThrew);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outThrew) {
            *outThrew = true;
        }
        return 0;
    }
}

DropRowIds DropListItemAt(void* pBook, int page, int slot) {
    DropRowIds out;
    if (!pBook || page < 0 || slot < 0) {
        return out;
    }
    void* pPages = SafeReadPtr(pBook, kOff_DropList);
    const int nPages = SafeArrayCount(pPages);
    if (nPages <= 0 || nPages > kMaxPagesPerTab || page >= nPages) {
        return out;
    }
    void* pEntries = SafeIndexPtr(pPages, page);
    const int nEntries = SafeArrayCount(pEntries);
    if (nEntries <= 0 || nEntries > kMaxEntriesPerPage || slot >= nEntries) {
        return out;
    }
    void* pRec = SafeIndexPtr(pEntries, slot * 2 + 1);
    if (!pRec) {
        return out;
    }
    out.haveRecord = true;

    const int decoded =
            FuseGetGuarded(reinterpret_cast<char*>(pRec) + kOff_DropRec_ItemFuse, &out.threw);
    // v83 item ids are 7 digits (1000000..5999999 across the categories); anything outside that is
    // a decode that went wrong, not an item, and must fall through to the WZ ordinal rather than be
    // sent to the server.
    if (decoded >= 1000000 && decoded < 10000000) {
        out.fuse = decoded;
    }
    return out;
}

// `CItemInfo::GetItemName(out, itemId)` -- J-A, rewritten to the PROVEN signature at the PROVEN
// address (see the kAddr_CItemInfo_GetItemName block). Cached for the life of one book. Used to put
// a readable name beside every resolved id in the log (I1.3) and as the I1.7 tooltip fallback title.
std::unordered_map<int, std::string> g_itemName;
unsigned g_itemNameGen = 0;

// The raw call, in its own frame so nothing here needs C++ unwinding and the buffer really is a
// bare 4-byte slot -- the same `[ebp-0x14]` shape the client's own call site uses. `out` is written
// through EXACTLY once, 4 bytes, by 0x004181C9 inside the worker, so there is nothing past it to
// corrupt: this is the whole fix for "Run-Time Check Failure #2 -- stack around 'out' corrupted".
//
// The copy-out and the release are the client's own tail (0x00424859..0x0042486C) transcribed: read
// the char*, copy the characters, then `_Release(chars - 12)` through 0x00428D13 and drop the slot.
// Nothing is left holding a reference, which is the leak the spec called out.
int ItemNameRaw(int itemId, char* out, int cap) {
    if (!out || cap <= 0) {
        return 0;
    }
    out[0] = '\0';
    __try {
        void* pInfo = *reinterpret_cast<void**>(kAddr_CItemInfo_Instance);
        if (!pInfo) {
            return 0;
        }
        char* chars = nullptr; // the ZXString<char> handle -- 4 bytes, and 4 is all it writes
        reinterpret_cast<void*(__thiscall*)(void*, char**, int)>(kAddr_CItemInfo_GetItemName)(
                pInfo, &chars, itemId);
        int n = 0;
        if (chars) {
            while (n < cap - 1 && chars[n] != '\0') {
                out[n] = chars[n];
                ++n;
            }
            reinterpret_cast<void(__cdecl*)(void*)>(kAddr_ZXString_Release)(
                    chars - kOff_ZXStringHeader);
            chars = nullptr;
        }
        out[n] = '\0';
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return -1;
    }
}

std::string ItemNameInner(int itemId) {
    char buf[128];
    const int n = ItemNameRaw(itemId, buf, static_cast<int>(sizeof(buf)));
    if (n < 0) {
        return std::string();
    }
    return std::string(buf, static_cast<size_t>(n));
}

std::string ItemName(int itemId) {
    if (itemId <= 0) {
        return std::string();
    }
    if (g_itemNameGen != g_bookGen) {
        g_itemNameGen = g_bookGen;
        g_itemName.clear();
    }
    const auto it = g_itemName.find(itemId);
    if (it != g_itemName.end()) {
        return it->second;
    }
    std::string name = ItemNameInner(itemId);
    g_itemName[itemId] = name;
    return name;
}

// =================================================================================================
// H1.9 -- mob level, following info/link, cached for the life of one book
//
// `Mob/%07d.img/info/level`. Many mobs are art stubs whose `info` carries only `link` (the id of
// the mob that owns the real data), so a single hop is followed -- the same rule the rest of the
// pipeline uses. One WZ probe per mob per book; a miss is cached as 0 so a stub cannot be probed
// once per mouse move.
// =================================================================================================
std::unordered_map<int, int> g_mobLevel;
unsigned g_mobLevelGen = 0;

int MobLevel(int mobId) {
    if (mobId <= 0) {
        return 0;
    }
    if (g_mobLevelGen != g_bookGen) {
        g_mobLevelGen = g_bookGen;
        g_mobLevel.clear();
    }
    const auto it = g_mobLevel.find(mobId);
    if (it != g_mobLevel.end()) {
        return it->second;
    }
    int level = 0;
    try {
        int id = mobId;
        for (int hop = 0; hop < 2 && level == 0; ++hop) {
            wchar_t path[64];
            _snwprintf_s(path, _countof(path), _TRUNCATE, L"Mob/%07d.img/info", id);
            IWzPropertyPtr pInfo = get_object_or_empty(path).GetUnknown();
            if (!pInfo) {
                break;
            }
            level = get_int32(get_item_or_empty(pInfo, L"level"), 0);
            if (level > 0) {
                break;
            }
            const int link = get_int32(get_item_or_empty(pInfo, L"link"), 0);
            if (link <= 0 || link == id) {
                break;
            }
            id = link;
        }
    } catch (...) {
        level = 0;
    }
    if (level < 0 || level > 999) {
        level = 0;
    }
    g_mobLevel[mobId] = level;
    return level;
}

// The mob a GRID SLOT is showing (not the SELECTED one -- +0x5B8 answers that, and it is the wrong
// question for a hover).
//
// K5.1 -- while a result view owns the grid this now goes through the SAME `ResultCardIndexAt` /
// `ResultGridPage` pair the GetSlotCard remap and the drop-% overlay use, so the tooltip cannot
// resolve a slot differently from the card that was drawn in it. It also SAYS SO when it comes up
// empty: a hover that resolves nothing used to be indistinguishable from a hover that never
// happened, and "the tooltip is missing in the droppers view only" is precisely the report that
// needed telling apart.
int GridSlotMobId(void* pBook, int slot) {
    if (slot < 0 || slot >= kGridSlots) {
        return 0;
    }
    if (g_st.cardView) {
        if (g_cards.empty() || g_cardsForBook != pBook) {
            return 0;
        }
        const int page = ResultGridPage(pBook);
        const int mobId = ResultMobAt(page, slot);
        return mobId;
    }
    const int page = SafeReadInt(pBook, kOff_CardPage);
    if (page < 0) {
        return 0;
    }
    const int tab = LeftTabOrUnknown(pBook);
    if (tab < 0 || tab == kTabNoCard) {
        return 0;
    }
    return CardRecordMobAt(pBook, tab, page, slot);
}

// Mob name for a hover. The card index carries every name already (BuildCardIndex lower-cases them
// for the search); this needs the display spelling, so String/Mob.img is asked directly and the
// answer cached alongside the level.
std::unordered_map<int, std::string> g_mobName;
unsigned g_mobNameGen = 0;

std::string MobName(int mobId) {
    if (mobId <= 0) {
        return std::string();
    }
    if (g_mobNameGen != g_bookGen) {
        g_mobNameGen = g_bookGen;
        g_mobName.clear();
    }
    const auto it = g_mobName.find(mobId);
    if (it != g_mobName.end()) {
        return it->second;
    }
    std::string name;
    try {
        name = GetMobNameById(mobId);
        if (name == "Unknown") {
            name.clear();
        }
    } catch (...) {
        name.clear();
    }
    g_mobName[mobId] = name;
    return name;
}

// =================================================================================================
// I1.6 -- in the droppers-from-item view ONLY: the card's count digit becomes the drop %
//
// The type-2 reply now carries `{mobId, ppm}` per row (I2), so every card in that grid knows what
// the searched item's chance is for THAT mob. The digit the stock counter draws in the card's
// bottom-left corner is meaningless there -- it is how many of THAT CARD the player owns, which has
// nothing to do with the item they searched for -- so it is patched out and the percentage takes
// its place.
//
// This runs from the Redraw(pane 0) detour, i.e. AFTER 0x00863DF1 has painted the whole card grid
// (the grid loop at 0x008650B4 and the counter block at 0x00864E5A both live inside that function),
// on the same LEFT PAGE canvas and in the same frame. The card rects at book+0xB2C are already in
// that canvas's own space -- the +40/+25 translation is only needed to turn them into WINDOW space
// for hit-testing -- so the rect values are used verbatim.
// =================================================================================================
// (`g_dropperPpm` itself is declared up with the view teardowns, so every reset path can clear it.)

// Widest-first candidates, byte-identical to monsterBookDrops.cpp's BuildCandidates so the two
// label styles cannot drift: "<0.01%" for a real but sub-0.005% drop (printing "0.00%" for a drop
// that exists is a lie), two decimals otherwise, coarser forms only as fallbacks when the text
// would outgrow the card.
int BuildPpmCandidates(int ppm, char cand[3][16]) {
    const double pct = static_cast<double>(ppm) / 10000.0;
    int n = 0;
    if (ppm >= 1000000) {
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

// (K1: `SampleParchment` is DELETED with the digit patch it existed to colour. Nothing in this view
// paints over card art any more.)

void PaintDropperPercentsInner(void* pBook) {
    if (!g_st.cardView || !g_st.cardFromDroppers || g_dropperPpm.empty()) {
        return;
    }
    IWzCanvasPtr page = LayerCanvas(pBook, kOff_LeftPageLayer);
    if (!page) {
        return;
    }
    IWzFont* pFont = EnsureFont();
    // The bottom row's label would otherwise be asked to draw past the end of the layer. Read the
    // canvas's real height once and keep the label inside it; a canvas that will not answer gets
    // the conservative "no room below" treatment rather than a blind draw.
    int canvasH = 0;
    {
        unsigned h = 0;
        try {
            if (SUCCEEDED(page->get_height(&h)) && h > 0 && h < 4096u) {
                canvasH = static_cast<int>(h);
            }
        } catch (...) {
            canvasH = 0;
        }
    }
    // K5.1 -- the SAME page rule and the SAME slot resolver the grid, the hover and the GetSlotCard
    // remap use. (`GetSlotCard` is handed the client's own `book+0x5B0` at 0x00864914, which the
    // Update hook only forces back into agreement with `g_st.cardPage` AFTER the original Update --
    // and Redraw(0) runs INSIDE it, so for one frame after a page change the two can differ.)
    const int gridPage = ResultGridPage(pBook);
    const int slotsPerPage = ResultSlotsPerPage(); // K7(b) -- 20 here, i.e. four rows
    int drawn = 0;
    int noRoom = 0;
    int noPpm = 0;
    int noRect = 0;
    for (int slot = 0; slot < slotsPerPage; ++slot) {
        const int ci = ResultCardIndexAt(gridPage, slot);
        if (ci < 0) {
            continue; // past the end of this page's results -- the client drew no card here either
        }
        const int mobId = g_cards[static_cast<size_t>(ci)].mobId;
        const auto it = g_dropperPpm.find(mobId);
        if (it == g_dropperPpm.end() || it->second <= 0) {
            ++noPpm; // spec: no ppm -> draw NOTHING
            continue;
        }
        RECT rc;
        if (!ReadRect(pBook, kOff_CardRects, slot, rc)) {
            // Round 11: the other silent skip from this round's code. A card the client HAS drawn
            // whose rect will not read back means the label silently vanishes from under it, which
            // is indistinguishable on screen from "the server sent no ppm" -- so it is counted and
            // reported next to `noPpm` in the summary line below.
            ++noRect;
            continue;
        }

        // K1 -- NOTHING is painted over the card. The collected-count digit the client inserts at
        // (rect.left + 1, rect.bottom - digitHeight - 1) (0x0086501E..0x00865045) stays exactly as
        // it is on every other grid; this pass only adds a label UNDER the card.
        const int rcW = rc.right - rc.left;

        char cand[3][16];
        const int nCand = BuildPpmCandidates(it->second, cand);
        const char* text = cand[0];
        int textW = static_cast<int>(strlen(text)) * 6;
        if (pFont) {
            for (int c = 0; c < nCand; ++c) {
                int w = 0;
                try {
                    w = static_cast<int>(pFont->CalcTextWidth(cand[c]));
                } catch (...) {
                    w = 0;
                }
                if (w > 0 && w + kPpmLabelPadX * 2 <= rcW + 8) {
                    text = cand[c];
                    textW = w;
                    break;
                }
                if (w > 0 && c == 0) {
                    textW = w; // keep a real measurement even if nothing fits
                }
            }
        }
        const int boxW = textW + kPpmLabelPadX * 2;
        // K1 -- ALWAYS below the card, never on the digit.
        //
        // K7 -- and now there is always room. With 20 slots per dropper page the last row drawn is
        // row 3, whose rect bottom is 45*3+31+38 = 204, so the label occupies 205..217 inside a
        // 256 px layer. (Row 4 -- the one round 10 silently dropped -- would have wanted 250..262.)
        // This branch is therefore UNREACHABLE for every slot this loop now visits; it survives as
        // an assertion, not as a policy, and it says so in the log if a future geometry change ever
        // makes it fire again.
        const int boxY = rc.bottom + 1;
        if (boxY < 0 || (canvasH > 0 && boxY + kPpmLabelH > canvasH)) {
            ++noRoom;
            continue;
        }
        FillRect(page, rc.left, boxY, boxW, kPpmLabelH, kPpmLabelBg);
        DrawTextAt(page, rc.left + kPpmLabelPadX, boxY, text, pFont);
        ++drawn;
    }
}

void PaintDropperPercents(void* pBook) {
    __try {
        PaintDropperPercentsInner(pBook);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// =================================================================================================
// G0.4 / H1.3 -- navigation clears state
//
// A per-tick watcher on the RAW tab values:
//   * colour tier changed, or the book went to the cover (left tab 9)  -> everything cleared
//   * content tab changed while the ITEM view owns the page            -> the item view cleared
// A content-tab change while only the CARD view is up is legitimate (the right page belongs to the
// mob the player just picked out of the results), so the grid survives it.
//
// H1.3 ROOT CAUSE, and why the baseline is now split in two.
// Round 6 re-baselined BOTH tabs at the end of every OnMouseButton the book received. The colour
// strip is a `CCtrlTab` child, so a click on a tier is dispatched to the CONTROL -- but the button
// RELEASE still reaches the window (CUIMonsterBook::OnMouseButton's head runs on every message; its
// 0x202 path falls straight through to the tail at 0x008623BF). That release therefore ran our
// hook, which re-baselined `watchLeft` to the tier the player had just picked -- so the watcher
// never saw a change, the item view was never torn down, the GetSlotCard remap kept answering
// "tab 9 / no card" for every slot, and the tier click looked like it did nothing at all. Exactly
// the reported "tier switching is impossible while the item view is up".
//
// The RIGHT tab still has to be re-baselined, because `SetSelectedCard` forces it to 0 on our
// behalf (0x008679AE) whenever we drive a result pick. The LEFT one is only ever re-baselined by
// the two paths that PROVABLY moved it themselves: a card-result pick (SetSelectedCard switches to
// `record+8`, 0x00867970) and the H1.10 level search (which calls CCtrlTab::SetSelected itself).
// =================================================================================================
void RebaselineRightTab(void* pBook) {
    g_st.watchRight = RightTabOrUnknown(pBook);
}

void RebaselineTabs(void* pBook) {
    g_st.watchLeft = LeftTabOrUnknown(pBook);
    g_st.watchRight = RightTabOrUnknown(pBook);
}

// H1.3, second half: navigation clears the TEXTS too, not just the views. Both stashes and the live
// control, through the engine's own ZXString assignment.
void ClearSearchTexts(void* pBook, const char* reason) {
    const bool had = !g_st.rowText[0].empty() || !g_st.rowText[1].empty()
            || !ReadEditText(pBook).empty();
    g_st.rowText[0].clear();
    g_st.rowText[1].clear();
    g_st.rowPainted[0].clear();
    g_st.rowPainted[1].clear();
    const bool wrote = WriteEditText(pBook, std::string());
    if (pBook) {
        // K3.4 -- the strings are gone from the STATE; they are still on the SCREEN until something
        // repaints. Two different surfaces, two different mechanisms:
        //   row 1 lives on the left page, which only `Redraw(0)` rebuilds -> raise the pane flag,
        //         and invalidate the "already dirtied for" pair so the tick cannot decide the
        //         desired state is unchanged and skip it;
        //   row 0 lives on the window canvas, whose ONLY writer is `CWnd::Draw` on a transition ->
        //         nothing can schedule it, so it is painted here and now.
        g_st.rowDirtiedValid[0] = false;
        g_st.rowDirtiedValid[1] = false;
        g_st.hostDirtiedFor = -2;
        MarkDirty(pBook, 0);
        // Round 11: the edit has just been emptied by `WriteEditText` above, so there are no glyphs
        // left for the ordering guard to protect and this repaint goes through -- which is the whole
        // point of K3.4. If the write was refused, the guard defers instead of blanking live text.
        PaintWindowStripNow(pBook, "texts-cleared");
    }
}

void ClearLevelControl(void* pBook, const char* reason) {
    if (!g_st.levelOpen && g_st.levelSel < 0 && !g_st.levelView) {
        return;
    }
    g_st.levelOpen = false;
    g_st.levelSel = -1;
    g_st.levelView = false;
    if (pBook) {
        MarkAllDirty(pBook);
    }
}

// Everything a navigation event tears down, in one place so no caller can forget half of it.
void ClearAllSearchState(void* pBook, const char* reason) {
    ClearItemView(pBook, reason);
    ClearCardView(pBook, reason);
    ClearLevelControl(pBook, reason);
    ClearSearchTexts(pBook, reason);
    DropOurToolTip(pBook);
}

void WatchNavigation(void* pBook) {
    const int l = LeftTabOrUnknown(pBook);
    const int r = RightTabOrUnknown(pBook);
    if (g_st.watchLeft == -2) {
        g_st.watchLeft = l;
        g_st.watchRight = r;
        return;
    }
    if (l != kTabUnknown && l != g_st.watchLeft) {
        g_st.watchLeft = l;
        ClearAllSearchState(pBook, "left-tab-change");
    }
    if (r != kTabUnknown && r != g_st.watchRight) {
        // kRightTabNone is OUR park value; it can only be seen mid-Update, never from the tick.
        if (r != kRightTabNone) {
            g_st.watchRight = r;
            if (g_st.itemView) {
                ClearItemView(pBook, "right-tab-change");
                ClearSearchTexts(pBook, "right-tab-change");
                DropOurToolTip(pBook);
            }
        }
    }
}

// =================================================================================================
// H1.10 -- run the "cards per level" search
//
// Every card in the book whose CURRENT collected level equals the selection, ordered tier then card
// id, dropped into the existing GetSlotCard remap. `GetCardLevel` is called at its raw address on
// purpose: monsterBook.cpp DETOURS it (0x0095FC65) to retry `cardId % 10000`, which is the key the
// CHARSTATS login snapshot files owned cards under -- so going through the detour is what makes the
// count match what the grid itself draws.
//
// The tier switch afterwards is mandatory, not cosmetic: `CUIMonsterBook::Draw` diverts to the cover
// at 0x0086590E whenever the left tab reads 9, so a result set would never be painted from the home
// screen. It goes through `CCtrlTab::SetSelected` (0x008603DF) -- the same call SetSelectedCard uses
// at 0x00867970 -- and is followed by a left-tab re-baseline so our own navigation is not mistaken
// for the player's and does not immediately wipe the results we just built.
// =================================================================================================
void SwitchColourTier(void* pBook, int tier) {
    void* pTab = SafeReadPtr(pBook, kOff_LeftTabCtrl);
    if (!pTab || tier < 0 || tier >= kTabCount) {
        return;
    }
    __try {
        reinterpret_cast<void(__thiscall*)(void*, int)>(kAddr_CCtrlTab_SetSelected)(pTab, tier);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    __try {
        reinterpret_cast<void(__thiscall*)(void*, int)>(kAddr_Book_SetCardPage)(pBook, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    RefreshCtrlsNow(pBook);
    MarkAllDirty(pBook);
}

// Separate frame: MSVC refuses __try in a function that also needs C++ object unwinding, and the
// caller below owns a std::vector.
int SafeGetCardLevel(int cardId) {
    __try {
        return reinterpret_cast<int(__cdecl*)(int)>(kAddr_GetCardLevel)(cardId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void RunLevelSearchNow(void* pBook) {
    const int level = g_st.levelSel;
    if (level < kLvlMinLevel || level > kLvlMaxLevel) {
        return;
    }
    if (!BuildCardIndex(pBook)) {
        return;
    }
    std::vector<int> hits;
    hits.reserve(128);
    int probed = 0;
    int faulted = 0;
    for (size_t i = 0; i < g_cards.size(); ++i) {
        const int have = SafeGetCardLevel(g_cards[i].cardId);
        ++probed;
        if (have < 0) {
            ++faulted;
            continue;
        }
        if (have == level) {
            hits.push_back(static_cast<int>(i));
        }
    }
    // g_cards is already walked tier -> page -> index, i.e. tier order; sorting by (tab, cardId)
    // makes the second key explicit and survives any future change to the walk.
    std::sort(hits.begin(), hits.end(), [](int a, int b) {
        const CardLoc& la = g_cards[static_cast<size_t>(a)];
        const CardLoc& lb = g_cards[static_cast<size_t>(b)];
        if (la.tab != lb.tab) {
            return la.tab < lb.tab;
        }
        return la.cardId < lb.cardId;
    });

    g_st.levelOpen = false;
    ClearItemView(pBook, "level-search");
    SwitchColourTier(pBook, 0);
    OpenCardView(pBook, std::move(hits), "level-search");
    g_st.levelView = true;
    RebaselineTabs(pBook); // WE moved the left tab; the watcher must not read it as the player
}

// =================================================================================================
// the second magnifier (nId 0x7DA)
//
// Same recipe the book uses for its own buttons (0x008626BA alloc + 0x004258E4 ctor + CreateCtrl
// through primary slot 8) and the same one userList2.cpp already ships. `skinDesc` is the book's own
// 4-dword blob: desc[0] = 1 (the `push 1 / pop ebx` at 0x0086260C, stored at 0x0086267C and never
// rewritten -- it lands at ctrl+0x28, the ENABLED flag the sub-object getter 0x004259DD returns),
// desc[1] = desc[2] = 0, desc[3] = a live ZXString holding the art path.
//
// desc[3] must stay alive: an empty slot faults inside RESMAN, so the string is deliberately never
// destroyed. The control parents itself into the book's child list (0x004DFDA4), so the WINDOW owns
// and frees it; we only keep the pointer to show / hide it, and drop it the moment the book changes.
// =================================================================================================
ZXString<wchar_t>* g_btSearch2Art = nullptr;

const wchar_t* BtSearch2ArtString() {
    if (!g_btSearch2Art) {
        try {
            g_btSearch2Art = new ZXString<wchar_t>(kUOL_SearchButtonArt);
        } catch (...) {
            g_btSearch2Art = nullptr;
        }
    }
    if (!g_btSearch2Art) {
        return nullptr;
    }
    const wchar_t* p = static_cast<const wchar_t*>(*g_btSearch2Art);
    return (p && *p) ? p : nullptr;
}

void* CreateSearchButton2(void* pBook, const wchar_t* pArt) {
    if (!pBook || !pArt) {
        return nullptr;
    }
    void* btn = nullptr;
    __try {
        btn = reinterpret_cast<void*(__thiscall*)(void*, unsigned)>(kAddr_ZAlloc_Alloc)(
                reinterpret_cast<void*>(kAddr_ZAlloc_Instance), kSize_CCtrlButton);
        if (!btn) {
            return nullptr;
        }
        reinterpret_cast<void(__thiscall*)(void*)>(kAddr_CCtrlButton_Ctor)(btn);
        void** vt = *reinterpret_cast<void***>(btn);
        if (!vt || !vt[kVTIdx_CreateCtrlBtn]) {
            return nullptr;
        }
        DWORD skinDesc[4] = { 1, 0, 0, reinterpret_cast<DWORD>(pArt) };
        reinterpret_cast<int(__thiscall*)(void*, void*, unsigned, int, int, int, void*)>(
                vt[kVTIdx_CreateCtrlBtn])(
                btn, pBook, kNId_BtSearch2, kBtnX, kBtnY[kRowItem], 0, skinDesc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return btn;
}

// Re-armed per BOOK: up to kCtrlMaxAttempts spaced tries, every one logged with its outcome. There
// is no session-scoped "we tried once and gave up" any more.
void EnsureSecondMagnifier(void* pBook) {
    if (g_st.btSearch2 && !IsOurCtrl(g_st.btSearch2, pBook, kNId_BtSearch2)) {
        g_st.btSearch2 = nullptr;
    }
    if (g_st.btSearch2) {
        return;
    }
    if (g_st.ctrlAttempts >= kCtrlMaxAttempts) {
        // Once, not every tick -- and never for the session: the next OnCreate resets ctrlAttempts
        // with the rest of SearchState, so the next book tries again from scratch (6.G G0.2).
        if (g_st.ctrlAttempts == kCtrlMaxAttempts) {
            ++g_st.ctrlAttempts; // so this line is written exactly once per book
        }
        return;
    }
    const DWORD now = GetTickCount();
    if (g_st.ctrlNextTry != 0 && now < g_st.ctrlNextTry) {
        return;
    }
    g_st.ctrlNextTry = now + kCtrlRetryMs;
    ++g_st.ctrlAttempts;

    const wchar_t* pArt = BtSearch2ArtString();
    if (!pArt) {
        return;
    }
    void* btn = CreateSearchButton2(pBook, pArt);
    if (!btn) {
        return;
    }
    if (!IsOurCtrl(btn, pBook, kNId_BtSearch2)) {
        return;
    }
    g_st.btSearch2 = btn;
}

// -------------------------------------------------------------------------------------------------
// J-B round 10 -- THE WINDOW-DRAG WATCH.
//
// `[CWndMan+0x84]` is the window the UI manager is currently dragging. The dispatcher sets it when a
// 0x201 lands on a window whose HitTest answered 1 -- for this book, any point with window y in
// 0..24, i.e. the whole ITEM row / level box / level magnifier band (0x0092C604, 0x009E3F76) -- and
// then RETURNS, so the press's DOWN never reaches OnMouseButton. It re-tests the pointer on every
// later mouse message (0x009E3B55), repositions the window, and only when `IsKeyDown(VK_LBUTTON)`
// (0x0059A25A) says the button is up does it finish the drag: zero the pointer (0x009E3CA5) and fall
// through to the ordinary dispatch, which delivers that 0x202 to OnMouseButton at 0x009E3FBF.
//
// That last detail is exactly the round-9 defect "a window drag always ends by firing the top band":
// the release of a drag and the release of a click are the SAME message at the same rx/ry, and the
// drag flag is already cleared by the time the hook sees it. It cannot be read at the release -- but
// it can be read one frame earlier, from the pump, which is what this does.
//
// CRITICAL, and the reason the flag alone is not the answer: EVERY press in the band starts a drag,
// including the one the player means as a click. A click is simply a drag of zero distance. So the
// flag only says "a press is in progress up there"; what separates the two is whether the window
// actually MOVED while it was set. The dispatcher repositions the dragged window on every mouse
// message from the cursor delta (0x009E3C58..0x009E3C82 -> primary[5] with GetAbsLeft/GetAbsTop plus
// the delta), so for a click the origin never changes and for a drag it changes every frame. A few
// pixels of slack keeps a nudge or a one-pixel rounding artefact on the click side of the line --
// erring, as everywhere in this hook, towards "the click works".
constexpr DWORD kDragGraceMs = 150; // ~2 frames at 60 Hz; far shorter than any human re-click
constexpr int kDragSlopPx = 3;      // window travel below this is still a click
// How long a press origin may stand after the button reads physically up. Long enough that a real
// click's own 0x202 -- dispatched in the message pump that runs BEFORE the frame's Update -- always
// gets to clear the origin itself; short enough that a stranded origin never survives to refuse the
// next click. The absolute age cap in PressOriginOk (double-click time + 250 ms) is the backstop for
// a book whose Update is not pumping at all.
constexpr DWORD kPressButtonUpGraceMs = 150;
// The floor under both retirements, checked in PressOriginOk. Deliberately far longer than any
// press-and-drag a player performs (which must keep its origin, or its release over the colour strip
// is read as a strip click) and far shorter than "the rest of the session", which is what round 10
// shipped: the log's stranded origin was still refusing clicks 3.7 seconds later.
constexpr DWORD kPressOriginMaxAgeMs = 30000;

void* ReadWndManDragTarget() {
    __try {
        void* pMgr = *reinterpret_cast<void**>(kAddr_CWndMan_Instance);
        if (!pMgr) {
            return nullptr;
        }
        return *reinterpret_cast<void**>(reinterpret_cast<char*>(pMgr) + kOff_WndMan_Drag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Called once per frame from the Update pump.
void WatchWindowDrag(void* pBook) {
    const bool dragging = (pBook != nullptr) && (ReadWndManDragTarget() == pBook);
    int l = 0, t = 0;
    const bool haveOrigin = dragging && ReadAbsOrigin(reinterpret_cast<char*>(pBook) + 4, &l, &t);

    if (dragging && !g_st.dragActive) {
        // BEGIN. Anchor the window where the press found it and start over on "has it moved".
        g_st.dragMoved = false;
        g_st.dragAnchorOk = haveOrigin;
        g_st.dragAnchorX = l;
        g_st.dragAnchorY = t;
        // A NEW press has begun, so any origin still standing belongs to an OLDER one whose UP went
        // somewhere else. This is the exact shape of the shipped log's failure: `DRAG begin` at
        // 00:17:18.307 for a deliberate click on the level box, refused 106 ms later against the
        // (472,61) origin left over from 00:17:14.743. Retire it here rather than let the drag band
        // inherit it -- the press that is starting now owns the band.
        if (g_st.pressDownSeen) {
            g_st.pressDownSeen = false;
        }
    }
    // ...and the third retirement: the physical button is UP while an origin is still standing, so
    // whatever press recorded it has ended without its 0x202 coming back here. The grace keeps this
    // clear of the normal case -- the client pumps its messages before it pumps Update, so a release
    // whose UP IS delivered to this hook has already cleared the origin by the time this runs.
    if (g_st.pressDownSeen && (GetTickCount() - g_st.pressDownTick) > kPressButtonUpGraceMs
            && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
        g_st.pressDownSeen = false;
    }
    if (dragging) {
        g_st.dragTick = GetTickCount();
        const int dx = l - g_st.dragAnchorX;
        const int dy = t - g_st.dragAnchorY;
        if (haveOrigin && g_st.dragAnchorOk && !g_st.dragMoved
                && (dx > kDragSlopPx || dx < -kDragSlopPx || dy > kDragSlopPx
                        || dy < -kDragSlopPx)) {
            g_st.dragMoved = true;
        }
    }
    g_st.dragActive = dragging;

    // ROUND 11 -- AND `dragMoved` RETIRES ON ITS OWN.
    //
    // It was cleared in exactly one place: the `DRAG begin` branch above. So after a real drag it
    // stayed true until the NEXT press started, and `WindowDragOwnsThisPress` refuses a click for
    // `kDragGraceMs` after `dragTick` -- which means a genuine top-band click landing inside that
    // window right after a drag was thrown away with `originWhy=window-drag`. The grace exists to
    // cover the ONE release that the drag itself produces (the UI manager zeroes CWndMan+0x84 inside
    // the same call that then delivers the closing 0x202, so `dragActive` is still true when that UP
    // is handled and this frame-later observation is what tells the two apart). Once the grace has
    // elapsed there is nothing left to protect, so the flag goes down here -- exactly the same
    // window the refusal uses, so no release that SHOULD be refused ever escapes.
    if (!dragging && g_st.dragMoved && g_st.dragTick != 0
            && (GetTickCount() - g_st.dragTick) > kDragGraceMs) {
        g_st.dragMoved = false;
    }
}

// True when the 0x202 now being processed is the release of a real window drag rather than a click:
// the book was (or was a frame ago) the UI manager's drag target AND the window actually travelled.
bool WindowDragOwnsThisPress() {
    if (!g_st.dragMoved) {
        return false; // a zero-distance drag IS the click every band press has to be
    }
    return g_st.dragActive
            || (g_st.dragTick != 0 && (GetTickCount() - g_st.dragTick) <= kDragGraceMs);
}

// =================================================================================================
// hooks
// =================================================================================================

// Per-frame pump point, reached through the VTABLE slot because monsterBookDrops.cpp owns the
// Detours attach on this address -- calling the raw address here runs that module's detour and then
// the original, so the two passes stay strictly ordered and neither is skipped.
void __fastcall CUIMonsterBook_Update_hook(void* pThis, void* edx) {
    MUSH_FEATURE("monsterBookSearch:update");
    if (!pThis) {
        return;
    }
    if (g_st.book != pThis) {
        // A book we have not seen: OnCreate normally gets there first, but a book that existed
        // before this module attached would otherwise never be adopted.
        ResetForClosedBook("adopt-new-book");
        g_st.book = pThis;
        RebaselineTabs(pThis);
    }
    g_st.seenTick = GetTickCount();
    // J-B round 10 -- sample the UI manager's drag pointer while it is still set. It is zeroed
    // inside the very call that then delivers the drag's closing 0x202 to OnMouseButton, so this
    // frame-earlier read is the only place the two can still be told apart.
    WatchWindowDrag(pThis);

    // The item view owns the right page for as long as it has results AND the book is on a card
    // screen (left tab 9 paints no right page at all -- Draw diverts at 0x0086590E).
    const bool wantItems = g_st.itemView && !g_st.itemResults.empty() && OnTierScreen(pThis);
    if (wantItems != g_st.itemViewLive) {
        g_st.itemViewLive = wantItems;
        MonsterBookSearch_SetItemResultView(wantItems);
        if (wantItems) {
            if (kParkTabStripForItemView) {
                ParkTabStrip(pThis, "item-view");
            }
            // K4 -- the item results ARE a Dropping list, so the strip's stored SELECTION goes to 2
            // through the client's own setter (`CCtrlTab::SetSelected`, 0x008603DF -- the same call
            // SetSelectedCard uses at 0x00867970), which is what makes the "Dropping" tab light up
            // instead of "Basic Info". The per-Update park to index 4 (ItemViewChrome) is untouched:
            // it is a raw write to ctrl+0x34 that is paired inside one Update and never reaches the
            // control's own bookkeeping, so it cannot disturb this selection.
            //
            // The setter notifies the parent synchronously (0x008606AF -> OnChildNotify(0x7D7,
            // 0x1F4) -> SetListPage(0) + RefreshCtrls at 0x00861FA4..0x00861FB6), which is harmless
            // here and is exactly what a real click on that tab would have done -- but the change is
            // OURS, so the navigation watcher is re-baselined or it would read it as the player
            // leaving the item view and tear the view down on the very frame it opened.
            void* pRightTab = SafeReadPtr(pThis, kOff_RightTabCtrl);
            const int before = RightTabOrUnknown(pThis);
            if (pRightTab && before != kTabDropping) {
                CallTabSetSelected(pRightTab, kTabDropping, "item-view/right");
                // The park state is logged NEXT TO the selection on purpose -- see
                // kParkTabStripForItemView. While the strip is parked (`parked=1`, x = home+3000,
                // SetShow(0)) this selection is stored but cannot be seen, which is K4's stated
                // purpose and 6.G-G0.5's requirement contradicting each other. One line of evidence
                // per view opening beats another round of remembering.
            }
            RebaselineRightTab(pThis);
        } else {
            // I1.4: leaving the item view only un-parks the strip when the book is NOT on the home
            // screen -- home holds a park of its own (reason=home) and the tick re-asserts it. The
            // other two restorations are unconditional: the borrowed pagers and any tooltip the
            // view raised belong to a page that no longer exists, whichever screen we ended on.
            if (!OnHomeScreen(pThis)) {
                UnparkTabStrip(pThis);
            }
            EnforceListArrows(pThis, false);
            DropOurToolTip(pThis);
        }
        MarkAllDirty(pThis); // the LEFT grid blanks with it too, so both pages repaint
    }

    if (g_st.itemViewLive) {
        ItemViewChrome chrome(pThis); // parks the content tab index and lends the header our caption
        CUIMonsterBook_Update_Orig(pThis, edx);
    } else {
        CUIMonsterBook_Update_Orig(pThis, edx);
    }

    // A result view owns the card page: SetSelectedCard (0x0086793F) navigates to the clicked card's
    // REAL page, which would slide our window of results out from under the grid.
    if (g_st.cardView) {
        const int page = SafeReadInt(pThis, kOff_CardPage);
        if (page != g_st.cardPage) {
            SafeWriteInt(pThis, kOff_CardPage, g_st.cardPage);
            MarkDirty(pThis, 0);
        }
    }
    if (g_st.itemViewLive) {
        PaintItemResultsGuarded(pThis);
    }
    // K2.2 -- the home screen's level box has no dirty flag to ride, so a change to `levelSel`,
    // `levelOpen` or the magnifier hover is painted right here, in the same frame it happened.
    EnsureLevelBoxFresh(pThis);
}

// The remap that turns the book's own card grid into the result view. Every substitution is another
// (tab,page,index) the book itself owns, so the original still hands back a genuine, refcounted
// record and everything downstream -- art, counter, click, SetMobInfo -- is stock behaviour.
void* __fastcall CUIMonsterBook_GetSlotCard_hook(
        void* pThis, void* edx, void* out, int tab, int page, int index) {
    if (pThis != g_st.book) {
        return CUIMonsterBook_GetSlotCard(pThis, edx, out, tab, page, index);
    }
    // While the item hits own the book, the LEFT page is theirs as well: it must not keep showing
    // whichever colour tier the book happened to be parked on. Asking for tab 9 is the ORIGINAL's
    // own "there is no card here" answer (0x00867A65 writes out[4] = 0 and returns), so this is a
    // genuinely empty grid, not a fabricated record. A dropper list overrules it -- that IS the
    // item view's left page.
    if (g_st.itemViewLive && !g_st.cardView) {
        return CUIMonsterBook_GetSlotCard(pThis, edx, out, kTabNoCard, 0, 0);
    }
    if (!g_st.cardView) {
        return CUIMonsterBook_GetSlotCard(pThis, edx, out, tab, page, index);
    }
    // K5.2 -- THE PICK IN FLIGHT OWNS THE (0,0) QUERY.
    //
    // While a card click is being dispatched, exactly one caller asks for (page 0, index 0): the
    // colour-tab notify's `SelectCurrentSlot` (0x00867923), reached from `OnChildNotify(0x7D6,
    // 0x1F4)` (0x00861FD0) AFTER it has zeroed +0x5B0, +0x5B8 and +0x5B4. Left alone it resolves to
    // `cardResults[0]` and the client fully selects the grid's FIRST card -- the reported "the first
    // click opens the wrong mob", and visible in the shipped log as a `DROPS SETMOBINFO mob=<first
    // card>` immediately before the correct one. Answering it with the CLICKED card makes the
    // client's own machinery select the right mob the first time. The other two queries of the same
    // dispatch cannot collide: the hit test asks (pickPage, pickSlot) -- identical to the override
    // when that is (0,0) -- and `GetSelectedCard` asks with +0x5B8 == -1, which is refused below.
    if (g_st.pickArmed && page == 0 && index == 0 && g_st.pickCardIndex >= 0
            && g_st.pickCardIndex < static_cast<int>(g_cards.size())) {
        const CardLoc& l = g_cards[static_cast<size_t>(g_st.pickCardIndex)];
        return CUIMonsterBook_GetSlotCard(pThis, edx, out, l.tab, l.page, l.index);
    }
    if (page >= 0 && index >= 0 && index < kGridSlots) {
        const int ci = ResultCardIndexAt(page, index);
        if (ci >= 0) {
            const CardLoc& l = g_cards[static_cast<size_t>(ci)];
            return CUIMonsterBook_GetSlotCard(pThis, edx, out, l.tab, l.page, l.index);
        }
    }
    return CUIMonsterBook_GetSlotCard(pThis, edx, out, kTabNoCard, 0, 0);
}

// -------------------------------------------------------------------------------------------------
// K5.2 -- arm / disarm the pick, around ONE call to the client's own mouse handler.
//
// Arming also writes `+0x5B8 = -1`. That is not a hack to dodge the override: `GetSelectedCard`
// (0x00867A2F) feeds `GetSlotCard(tab, +0x5B0, +0x5B8)` and the caller compares its answer with the
// hit record (`cmp edi,[eax+4] / setne` at 0x00862376) -- if they match, `SetSelectedCard` is
// SKIPPED and the click does nothing at all. A -1 makes the remap refuse (index < 0 -> the tab-9
// "no card here" answer, `out[4] = 0` at 0x00867A65), so the comparison always differs and a click
// on the already-selected card is honoured too. Both indices are written back by the caller after
// the original returns.
// -------------------------------------------------------------------------------------------------
void ArmCardPick(void* pBook, int gridSlot) {
    const int page = ResultGridPage(pBook);
    const int ci = ResultCardIndexAt(page, gridSlot);
    g_st.pickPage = page;
    g_st.pickSlot = gridSlot;
    g_st.pickCardIndex = ci;
    g_st.pickArmed = (ci >= 0);
    if (!g_st.pickArmed) {
        return;
    }
    const int prevSel = SafeReadInt(pBook, kOff_SelSlot);
    SafeWriteInt(pBook, kOff_SelSlot, -1);
    const CardLoc& l = g_cards[static_cast<size_t>(ci)];
}

void DisarmCardPick(void* pBook) {
    if (!g_st.pickArmed) {
        return;
    }
    // The -1 sentinel must never outlive the dispatch. `SetSelectedCard` overwrites it in every
    // normal path (0x008679A9 SetSelSlot(ord % 25)), but it is skipped entirely when the hit test
    // itself resolved nothing -- so the value is checked rather than assumed.
    const int sel = SafeReadInt(pBook, kOff_SelSlot);
    if (sel < 0) {
        SafeWriteInt(pBook, kOff_SelSlot, g_st.pickSlot);
    }
    g_st.pickArmed = false;
    g_st.pickCardIndex = -1;
    g_st.pickSlot = -1;
}

// H1.10 -- which part of the home-screen level control a WINDOW-space point lands in.
enum LevelHit { kLvlHit_None = -1, kLvlHit_Box = 0, kLvlHit_Button = 1, kLvlHit_Row = 2 };

int HitLevelControl(void* pBook, int rx, int ry, int* outLevel) {
    if (!OnHomeScreen(pBook)) {
        return kLvlHit_None; // I1.2 -- this control does not exist anywhere else
    }
    // I1.5, same overlap rule the field rows obey: the colour strip's control rect is x -7..49 and
    // this control's box and dropdown both start at x 48, so columns 48 and 49 are claimed twice.
    // The strip wins -- it is the claim the player cannot move, and a tier click that dies is worse
    // than two columns of a dropdown row that do not.
    if (HitLeftTabStrip(pBook, rx, ry)) {
        return kLvlHit_None;
    }
    const int listY = LevelListY(); // K2.1 -- the same answer the paint pass used
    if (g_st.levelOpen && PtIn(rx, ry, kLvlListX, listY, kLvlListW, kLvlListH)) {
        const int idx = (ry - listY) / kLvlRowH;
        if (idx >= 0 && idx < kLvlCount) {
            if (outLevel) {
                *outLevel = kLvlMinLevel + idx;
            }
            return kLvlHit_Row;
        }
        return kLvlHit_None;
    }
    if (PtIn(rx, ry, kLvlBoxX, kLvlBoxY, kLvlBoxW, kLvlBoxH)) {
        return kLvlHit_Box;
    }
    // I1.2 -- on the home screen the magnifier rect is ALWAYS ours: BtSearch2 has been parked
    // off-screen by the tick, so there is no control here to route the click to and no second owner
    // for these pixels. If the park somehow failed the click reaches BtSearch2 instead, and
    // OnButtonClicked refuses 0x7DA off a tier screen -- so the worst case is a dead click, never
    // the wrong search.
    if (PtIn(rx, ry, kLvlBtnX, kLvlBtnY, kLvlBtnW, kLvlBtnH)) {
        return kLvlHit_Button;
    }
    return kLvlHit_None;
}

// The colour strip's own CreateCtrl rect, `{-7, 25, 50, 305}` for nId 0x7D6 -- used only when the
// live control cannot be read, so an unreadable strip degrades to "the tier band is still off
// limits to the field rows" rather than to "the field rows may claim it".
constexpr int kLeftStripX = -7;
constexpr int kLeftStripY = 25;
constexpr int kLeftStripW = 57;  // 50 - (-7)
constexpr int kLeftStripH = 280; // 305 - 25

bool HitLeftTabStrip(void* pBook, int rx, int ry) {
    void* pTab = SafeReadPtr(pBook, kOff_LeftTabCtrl);
    int x = kLeftStripX, y = kLeftStripY, w = kLeftStripW, h = kLeftStripH;
    if (pTab) {
        int lx = 0, ly = 0;
        const int lw = SafeReadInt(pTab, kOff_Ctrl_Width);
        const int lh = SafeReadInt(pTab, kOff_Ctrl_Height);
        if (CtrlReadPos(pTab, &lx, &ly) && lw > 0 && lw <= 512 && lh > 0 && lh <= 512) {
            x = lx;
            y = ly;
            w = lw;
            h = lh;
        }
    }
    return PtIn(rx, ry, x, y, w, h);
}

// -------------------------------------------------------------------------------------------------
// J-B round 10 -- PRESS IDENTITY. Three small predicates, used by every UP-driven action.
// -------------------------------------------------------------------------------------------------

// Do two WINDOW-space points address the same thing? Every claim this hook can make about a point is
// compared, so a press that began on a card and ended on the item field, or began on the mob field
// and ended on the colour strip, is never mistaken for a click on where it ENDED.
bool SamePressTarget(void* pBook, int ax, int ay, int bx, int by) {
    if (HitLeftTabStrip(pBook, ax, ay) != HitLeftTabStrip(pBook, bx, by)) {
        return false;
    }
    if (HitFieldRow(pBook, ax, ay) != HitFieldRow(pBook, bx, by)) {
        return false;
    }
    int la = -1, lb = -1;
    const int ha = HitLevelControl(pBook, ax, ay, &la);
    const int hb = HitLevelControl(pBook, bx, by, &lb);
    if (ha != hb) {
        return false;
    }
    return ha != kLvlHit_Row || la == lb; // two different dropdown rows are two different targets
}

// The double-click suppression, with its own expiry. `pressConsumed` exists only to swallow the UP
// that trails a 0x203 inside ONE gesture, so it cannot outlive the double-click window; anything
// longer than that is a stuck latch, which is precisely the round-9 defect being repaired.
bool PressConsumedNow() {
    if (!g_st.pressConsumed) {
        return false;
    }
    if ((GetTickCount() - g_st.pressConsumedTick) > (GetDoubleClickTime() + 250)) {
        g_st.pressConsumed = false;
        return false;
    }
    return true;
}

// May the release at (rx,ry) be treated as a click on (rx,ry)? Only positive evidence rejects, so a
// press this module never saw the start of is still a click: dropping a genuine click is the failure
// this whole round exists to remove, and "the drag also clicked" is the milder of the two.
//   * the book was being dragged by its caption a frame ago -> this UP closed the drag, not a click;
//   * a 0x201 for this press DID reach us and it was somewhere else -> a press-and-drag release.
bool PressOriginOk(void* pBook, int rx, int ry, const char** outWhy) {
    if (WindowDragOwnsThisPress()) {
        if (outWhy) {
            *outWhy = "window-drag";
        }
        return false;
    }
    // ...and only while it is still THIS press's origin. A DOWN whose UP never comes back here (the
    // dispatcher re-resolves the target from the cursor at 0x009E42B2 and drops the message outright
    // when the child under the release point is disabled or hidden -- 0x009E43C5 / 0x009E43CC /
    // 0x009E3E8C) used to leave the origin standing for the rest of the session, and only positive
    // evidence may reject a click.
    //
    // The two retirements that actually settle it live in `WatchWindowDrag`, one frame-accurate and
    // one evidence-based: a new DRAG begin, and the left button reading physically up with no 0x202
    // having arrived. A plain elapsed-time cap here -- `pressConsumed`'s rule -- was deliberately NOT
    // used as the primary: a press-and-hold that drags for a second or two is indistinguishable from
    // a stale origin by age alone, so the cap would re-authorise exactly the release-over-the-strip
    // teardown J-B removed. It survives only as a floor no gesture can reach.
    if (g_st.pressDownSeen && (GetTickCount() - g_st.pressDownTick) > kPressOriginMaxAgeMs) {
        g_st.pressDownSeen = false;
    }
    if (g_st.pressDownSeen && !SamePressTarget(pBook, g_st.pressDownX, g_st.pressDownY, rx, ry)) {
        if (outWhy) {
            *outWhy = "moved-off-origin";
        }
        return false;
    }
    return true;
}

// -------------------------------------------------------------------------------------------------
// Mouse buttons. Reached through the vtable slot because monsterBookFoundIn.cpp owns the Detours
// attach on 0x00862184.
//
// EVERY interaction below is a SINGLE CLICK (6.G G0.3) -- the round-5 double-click gate is gone,
// which is what stopped an item result from opening its droppers. Which MESSAGE of that click it
// runs on is now decided by WHERE it is, and that is J-B:
//
//   (round 10 note: the band is really window y 0..24, and the reason is the book's own HitTest
//    override at 0x0092C5E4 -- it answers 1 for an unclaimed point with y < 0x19 and the dispatcher
//    turns that into a window drag and swallows the DOWN at 0x009E3F76. Every press up there is a
//    drag; a click is one of zero distance, which is exactly how the two are told apart now.)
//
//   window y 9..25  (the ITEM field, the level box, the level magnifier)   -> 0x202 WM_LBUTTONUP
//   window y 46..130 (the level dropdown rows)                             -> 0x202 WM_LBUTTONUP
//   everything else (the MOB field, the grids, the strip)                  -> 0x201 WM_LBUTTONDOWN
//
// because the row-0 band NEVER receives 0x201 -- the window-drag capture takes it, proven by every
// CLICKRAW line of the round-9 session. WM_LBUTTONDOWN remains the message the client's own
// card-grid selection runs on (0x008621D4 `sub eax,0x201 / je 0x862345`), so the clicks that are
// still on DOWN and the book's own agree exactly as before.
//
// H1.1 ROOT CAUSE -- why the item field needed a second click or the magnifier.
// `CUIMonsterBook::OnMouseButton`'s HEAD runs for EVERY message, not just the ones it dispatches:
//
//     ecx = [book+0xAE8];  if (!ecx || !CCtrlWnd::IsHit(ecx, rx, ry)) {
//         [book+0x64] = 0;  CWndMan::SetFocus(book+4);          // 0x008621B9..0x008621CC
//     }
//
// and `IsHit` (0x004DFECE) tests the control-LOCAL rect {0,0,120,15} against WINDOW-space rx/ry.
// Round 6 swallowed the WM_LBUTTONDOWN on a field row and focused the edit -- and then handed the
// matching WM_LBUTTONUP straight to the original, whose head promptly took the focus back to the
// window (0x202 has no dispatch of its own: `sub eax,0x201 / je` then `sub eax,4 / jne` drops it at
// the tail). Only clicks in the top 15 px of a row survived, which is why typing "flapped" between
// working and leaking to the global hotkeys, and why a SECOND click always worked: by then the edit
// was physically on that row, so CWnd::HitTest routed the message to the control and the book never
// saw it. Every button message over a field row is therefore consumed here now.
// -------------------------------------------------------------------------------------------------
void __fastcall CUIMonsterBook_OnMouseButton_hook(
        void* pThis, void* edx, unsigned int msg, unsigned int wParam, int rx, int ry) {
    MUSH_FEATURE("monsterBookSearch:mousebutton");
    void* pBook = pThis ? reinterpret_cast<char*>(pThis) - 4 : nullptr;
    if (!pBook || pBook != g_st.book) {
        CUIMonsterBook_OnMouseButton_Orig(pThis, edx, msg, wParam, rx, ry);
        return;
    }

    const bool bDown = (msg == WM_LBUTTONDOWN) || (msg == WM_LBUTTONDBLCLK);
    // ROUND 11 -- and the SINGLE-FIRE version of it.
    //
    // Windows delivers a double-click as 0x201, 0x202, **0x203**, 0x202 -- the third message
    // REPLACES the second press's 0x201, it does not accompany it. `bDown` treats 0x203 as another
    // press, so one double-click on a drop icon or on an item result ran `RequestDroppers` twice:
    // two 0x372B queries, two `VIEW card ON` teardowns and two rebuilds of the same grid, the second
    // of which lands while the first is still in flight. Everything that is NOT idempotent therefore
    // acts on the plain 0x201 only. It costs nothing in reach: the client's own card-grid selection
    // is also 0x201-only (`sub eax,0x201 / je 0x00862345` at 0x008621D4), so a message this refuses
    // was never going to select anything either. The two focus paths keep `bDown`, because asserting
    // focus twice is the definition of idempotent.
    const bool bPrimaryDown = (msg == WM_LBUTTONDOWN);
    const bool bAnyButton = (msg == WM_LBUTTONDOWN) || (msg == WM_LBUTTONDBLCLK)
            || (msg == WM_LBUTTONUP) || (msg == WM_RBUTTONDOWN) || (msg == WM_RBUTTONUP)
            || (msg == WM_RBUTTONDBLCLK);

    // -------------------------------------------------------------------------------------------
    // J-B -- THE DRAG BAND. Row 0 and the whole level control fire on 0x202 UP, row 1 on 0x201 DOWN.
    //
    // Round 9's CLICKRAW lines showed it; round 10 proved it. `CUIMonsterBook`'s HitTest override
    // (primary[9] = 0x0092C5E4) answers 1 -- "drag me" -- for any point with window y in 0..24 that
    // no child control claims (0x0092C604 `cmp [ebp+0xc], 0x19`), and the UI dispatcher turns that 1
    // into `[CWndMan+0x84] = this window` and an immediate return (0x009E3F76): OnMouseButton never
    // sees that DOWN. 0x203 and 0x202 are not HitTested at all and go straight through to it
    // (0x009E3FBF). So the ITEM row, the level box and the level magnifier receive only 0x203/0x202,
    // while row 1 at y 29..45 answers 2 and keeps its DOWN.
    //
    // `bTopAct` is therefore the action message for everything in that band -- but only when the UP
    // really is the end of a click that began there. Round 9 asked no such question and had one
    // module-global latch instead, which the MOB row's DOWN set and only a later DOWN could clear;
    // since the band delivers no DOWN, a single mob-field click left the item field and the whole
    // level control dead for the rest of the session. The three-part answer is above the hook:
    // `PressConsumedNow` (the double-click's trailing UP), `PressOriginOk` (press origin + window
    // drag), and the per-press bookkeeping right here.
    // -------------------------------------------------------------------------------------------
    if (msg == WM_LBUTTONDOWN) {
        // A genuinely new press, and one that got PAST the drag test, so its point is a real origin.
        g_st.pressDownSeen = true;
        g_st.pressDownTick = GetTickCount(); // ...for as long as it can still BE this press's origin
        g_st.pressDownX = rx;
        g_st.pressDownY = ry;
        g_st.pressConsumed = false;
    } else if (msg == WM_LBUTTONDBLCLK) {
        // NOT a new press: 0x203 is the SECOND click of one gesture, and its own UP follows. Round 9
        // treated it as a fresh press and cleared the latch here, so a double-click on the level box
        // toggled the dropdown open on the first UP and shut on the second -- the control read as
        // broken for the one gesture the player had been trained since round 3 to use. Marking the
        // press consumed suppresses that trailing UP; the origin is still recorded, because a
        // double-click that DRIFTS off its control is no more a click than a single one.
        g_st.pressDownSeen = true;
        g_st.pressDownTick = GetTickCount();
        g_st.pressDownX = rx;
        g_st.pressDownY = ry;
        g_st.pressConsumed = true;
        g_st.pressConsumedTick = GetTickCount();
    }
    const bool bWasConsumed = PressConsumedNow();
    const char* originWhy = nullptr;
    const bool bOriginOk = PressOriginOk(pBook, rx, ry, &originWhy);
    const bool bTopAct = (msg == WM_LBUTTONUP) && !bWasConsumed && bOriginOk;
    // Snapshot for the logs below: the UP clears the press bookkeeping a few lines from here, and a
    // CLICKRAW line that reported the already-cleared state would hide the very thing it is for.
    const bool bDownSeen = g_st.pressDownSeen;
    const int downX = g_st.pressDownX;
    const int downY = g_st.pressDownY;
    const DWORD downAge = bDownSeen ? (GetTickCount() - g_st.pressDownTick) : 0;
    if (msg == WM_LBUTTONUP) {
        // THE PRESS ENDS AT ITS UP. Round 9 cleared its latch on the next DOWN instead, and the band
        // has no DOWN to give -- that single line is what made the top strip a one-shot. Everything
        // above has already been read, so clearing here is safe for every path below, including the
        // dozen early returns.
        g_st.pressConsumed = false;
        g_st.pressDownSeen = false;
    }
    const int leftTab = LeftTabOrUnknown(pBook);
    const bool onCards = OnTierScreen(pBook);
    const bool onHome = OnHomeScreen(pBook);

    // I1.5 -- END-TO-END CLICK INSTRUMENTATION.
    //
    // "Typing in a field blocks colour-tier clicks" could not be reproduced from the disassembly,
    // and the reason it could not is worth writing down: the book's OnMouseButton is only ever
    // reached when `CWnd::HitTest` found NO child under the point. The proof is the head of this
    // very function (0x0086219B..0x008621CC): it hit-tests the EDIT with `IsHit(rx, ry)`, and
    // `CCtrlWnd::IsHit` (0x004DFECE) tests the control-LOCAL rect {0,0,120,15} against those
    // WINDOW-space coordinates -- so for the mob row at y 30..45 it is ALWAYS false and the client
    // ALWAYS steals focus to the window. The mob field nevertheless types in game, so its clicks
    // cannot be arriving here; they are being routed to the control. By the same argument a colour
    // strip click normally never reaches this hook at all.
    //
    // So this line exists to settle it with data rather than inference: every button message the
    // book actually receives is logged with the point, both tab reads, whether the point falls in
    // the strip band and in a field band, and who owns the focus. If the next repro shows tier
    // clicks arriving here, the strip is losing the hit test and (4) below is what saves them; if
    // it shows nothing at all for a tier click, the click reached the control and the problem is
    // downstream of this module entirely. Button messages are a few per second at most -- this is
    // deliberately NOT rate-limited.

    // (0) H1.10 -- the home screen's level control. Only left tab 9 can reach it.
    //     J-B: every one of its three parts now acts on `bTopAct` (the 0x202 UP), not on `bDown`.
    //     The box and the magnifier sit in the drag band and never see a 0x201 at all; the dropdown
    //     rows (y 46..130) do see one, and for them the UP is simply the other end of the same
    //     press -- the DOWN is still CONSUMED below, so the client never sees it either way.
    if (bAnyButton && onHome) {
        int level = -1;
        const int hit = HitLevelControl(pBook, rx, ry, &level);
        if (hit != kLvlHit_None) {
            if (bTopAct) {
                switch (hit) {
                case kLvlHit_Row:
                    // I1.8 -- picking writes `level N` into the field text IMMEDIATELY. The field
                    // text IS `LevelCaption`, which reads `levelSel` on every paint, so setting it
                    // here plus dirtying is the whole of it: no second copy of the string exists to
                    // fall behind. The SEARCH stays on the magnifier, per spec.
                    g_st.levelSel = level;
                    g_st.levelOpen = false;
                    MarkAllDirty(pBook);
                    break;
                case kLvlHit_Box:
                    // I1.8 -- ONE click opens it. (And one closes it: the same rect toggles, which
                    // is what a dropdown does everywhere else in this client.)
                    g_st.levelOpen = !g_st.levelOpen;
                    MarkAllDirty(pBook);
                    break;
                case kLvlHit_Button:
                    if (g_st.levelSel >= 0) {
                        g_st.wantLevelSearch = true; // the index build runs on the flush
                    }
                    g_st.levelOpen = false;
                    MarkAllDirty(pBook);
                    break;
                default:
                    break;
                }
                // Round 9 set the press latch here, inside `if (bTopAct)` and under
                // `if (msg != WM_LBUTTONUP)` -- provably dead, because `bTopAct` already requires
                // WM_LBUTTONUP. Deleted rather than repaired: nothing needs to latch here. `bTopAct`
                // is true for exactly one message per press (the UP), so the double-fire the spec
                // asks to be guarded against is structurally impossible, and a client that did start
                // delivering 0x201 into this band would still act only on the UP.
            }
            return; // ours, in every direction -- the head's focus steal must not run
        }
        if (bTopAct && g_st.levelOpen) {
            // A click anywhere else on the home screen rolls the list back up (the spec's
            // "ESC/other-click closes it"); the click itself still reaches the client. On the UP,
            // like every other level-control action, so it cannot roll the list up on the DOWN of
            // the very press that was about to pick a row.
            g_st.levelOpen = false;
            MarkAllDirty(pBook);
        }
    }

    // (1) A FIELD ROW. Handled entirely here for EVERY button message, so the head's unconditional
    //     focus steal can never undo the focus we just set (see the block comment above), and the
    //     click can never also select a card underneath (the mob row overlaps the card page).
    if (bAnyButton && onCards) {
        const int row = HitFieldRow(pBook, rx, ry);
        if (row >= 0) {
            // J-B: row 0 (the ITEM field, window y 9..25) is in the drag band and never receives
            // 0x201, so it acts on the 0x202 UP. Row 1 (the MOB field, y 29..45) receives its 0x201
            // every time and keeps it -- moving row 1 to the UP as well would be a regression, since
            // its DOWN is the message the caret has always followed.
            const bool bAct = (row == kRowItem) ? bTopAct : bDown;
            if (bAct) {
                if (row != g_st.editRow) {
                    MoveEditToRow(pBook, row, true);
                } else {
                    const bool ok = FocusCtrl(StockEdit(pBook));
                }
                // Round 9 latched here too, with the comment "row 1 sets it harmlessly". It was the
                // ONLY live writer of that latch and it was the opposite of harmless: the MOB row is
                // the one row that acts on the DOWN, so every mob-field click armed a module-global
                // flag that only another DOWN could clear -- and the item field, the level box, the
                // dropdown and the level magnifier all live in the band that never delivers one.
                // The press bookkeeping at the head of this hook replaces it; row 1 latches nothing.
            } else if (msg == WM_LBUTTONUP && row == g_st.editRow) {
                // The release the original would have used to steal focus back. Re-assert instead;
                // FocusCtrl is a no-op when the edit already owns it (0x009E32DE).
                //
                // ONLY for the row that actually hosts the edit. Unconditionally re-asserting here
                // was the second half of the round-9 dead-field symptom: a refused release over the
                // ITEM row put the caret back on the MOB row's edit, so the field looked dead AND
                // the keystrokes visibly went to the other row. A release we refused must leave the
                // focus exactly where it was.
                FocusCtrl(StockEdit(pBook));
            }
            RebaselineRightTab(pBook);
            return;
        }
    }

    // Round 11: 0x201 ONLY -- see `bPrimaryDown`. Both branches below fire `RequestDroppers`, which
    // sends a packet and rebuilds a view; a double-click must not run them twice.
    if (bPrimaryDown) {
        // (2) A RESULT ICON while the item view owns the right page -> that item's droppers.
        if (g_st.itemViewLive) {
            const int slot = HitItemSlot(pBook, rx, ry);
            if (slot >= 0) {
                const int itemId = ItemAtSlot(slot);
                if (itemId > 0) {
                    RequestDroppers(itemId);
                }
                RebaselineRightTab(pBook);
                return; // the view owns this page; the original has nothing to do with the click
            }
        }

        // (3) I1.3 -- the SAME single click on a normal mob's own Dropping tab (user point 9).
        //     Three independent answers are computed EVERY time and all three are logged:
        //       fuse   = ZtlSecureFuse<int>::Get(record+0x0C) -- what the client drew that icon from
        //       plain  = [record+0x1C]                        -- what round 7 (and the stock hover) read
        //       reward = String/MonsterBook.img/<mob>/reward/<page*16+slot> -- the baked list
        //     The FUSE is preferred because the draw path (0x00865D68) and the list builder
        //     (0x0086744E) both use it; the reward ordinal is the fallback when the fuse threw or
        //     decoded something that is not a 7-digit item id. `plain` is never used -- only logged,
        //     so the next log proves the round-7 diagnosis rather than asserting it.
        if (!g_st.itemViewLive && onCards && RightTabOrUnknown(pBook) == kTabDropping) {
            const int slot = HitItemSlot(pBook, rx, ry);
            if (slot >= 0) {
                const int listPage = SafeReadInt(pBook, kOff_ListPage);
                const DropRowIds ids = DropListItemAt(pBook, listPage, slot);
                const int mobId = SelectedMobId(pBook);
                const bool paged16 = DropPagingIsSixteen();
                const int rewardId = (mobId > 0 && listPage >= 0 && paged16)
                        ? RewardItemAt(mobId, listPage * kItemSlots + slot)
                        : 0;
                const int itemId = (ids.fuse > 0) ? ids.fuse : rewardId;
                const char* src = (ids.fuse > 0) ? "fuse(record+0x0C)" : "reward-ordinal(WZ)";
                if (itemId > 0) {
                    RequestDroppers(itemId);
                    RebaselineRightTab(pBook);
                    return;
                }
                // could not resolve the item -> let the client have its normal click
            }
        }

    }

    // (4) I1.5 -- A COLOUR TIER CLICK. Two absolute guarantees, in this order:
    //
    //       (a) it is NEVER consumed. There is no `return` in this block and no path above it can
    //           reach here having claimed the point, because HitFieldRow now yields the overlap
    //           columns to the strip. Whatever the client would have done with this message, it
    //           still gets to do.
    //       (b) it clears EVERYTHING -- both views, the level filter, and both rows' texts and
    //           stashes -- UNCONDITIONALLY, not only when a view happens to be up. Round 7 gated
    //           the teardown on `cardView || itemView || levelSel >= 0 || levelOpen`, so a book
    //           carrying nothing but typed text kept that text across a tier change; the round-8
    //           requirement is "a colour-strip click must always reach the client AND clear both
    //           fields' texts+stashes+views", and `ClearAllSearchState` is all four.
    //
    //     Focus goes back to the WINDOW as well, which is precisely what the client's own head does
    //     for any click that misses the edit (`[book+0x64] = 0; CWndMan::SetFocus(book+4)` at
    //     0x008621B9). If a focused edit is what was interfering with the strip's dispatch -- the
    //     round-8 suspicion this file could not prove or disprove from the disassembly -- then
    //     dropping focus here is the one lever that addresses it, and it costs nothing if it is not.
    //
    //     J-B round 10: a RELEASE over the strip only counts when the press began on the strip.
    //     `bAnyButton` alone meant that pressing a card (or the mob field), dragging left and letting
    //     go over the colour column wiped both fields' text -- a teardown the player never asked for
    //     off a gesture that was not a strip click at all. Guarantee (a) is untouched either way:
    //     this block still never consumes the message, so a refused release is handed to the client
    //     exactly as before, minus the teardown.
    if (bAnyButton && HitLeftTabStrip(pBook, rx, ry) && (msg != WM_LBUTTONUP || bOriginOk)) {
        const bool hadAnything = g_st.cardView || g_st.itemView || g_st.levelSel >= 0
                || g_st.levelOpen || !RowText(pBook, 0).empty() || !RowText(pBook, 1).empty();
        if (hadAnything) {
            ClearAllSearchState(pBook, "tier-click");
        }
        FocusWindowSelf(pBook);
    }

    // (5) A card-grid click while a result view is up: the original selects the card (and navigates
    //     the book to that card's REAL page); we put our page back afterwards.
    //
    //     K5.2: and the pick is ARMED first, so the tab-change notify the client fires from inside
    //     `SetSelectedCard` resolves its (0,0) grid query to THIS card instead of to the result
    //     grid's first one. See ArmCardPick / the GetSlotCard hook.
    int gridSlot = -1;
    if (bPrimaryDown && g_st.cardView && onCards) {
        gridSlot = HitCardSlot(pBook, rx, ry);
        if (gridSlot >= 0) {
            ArmCardPick(pBook, gridSlot);
        }
    }

    CUIMonsterBook_OnMouseButton_Orig(pThis, edx, msg, wParam, rx, ry);
    DisarmCardPick(pBook);

    if (gridSlot >= 0) {
        // K7(b): the hand-written `page * 25 + slot` is gone -- this is the SAME resolver the grid
        // was drawn from, so a dropper page (20 slots) can never map a click one row out of step.
        const int ci = ResultCardIndexAt(g_st.cardPage, gridSlot);
        if (ci >= 0) {
            SafeWriteInt(pBook, kOff_CardPage, g_st.cardPage);
            SafeWriteInt(pBook, kOff_SelSlot, gridSlot);
            // The right page now belongs to that mob's Basic Info, so the item hits stand down.
            ClearItemView(pBook, "card-result-picked");
            MarkAllDirty(pBook);
        }
        // SetSelectedCard moved BOTH tabs on OUR behalf (it switches the colour strip to
        // `record+8`, 0x00867970, and forces the content tab to 0, 0x008679AE), so this is the one
        // path allowed to re-baseline the LEFT watch.
        RebaselineTabs(pBook);
        return;
    }
    RebaselineRightTab(pBook);
}

// -------------------------------------------------------------------------------------------------
// Hover.
//   * While the ITEM view is up the original is NOT called at all: it would walk the MOB's Dropping
//     list over the same rects and tooltip whatever record sits there (the `Green Apple` the tester
//     saw over an item-search icon). We answer the hover completely -- H1.6/8.
//   * Otherwise the original runs FIRST and keeps every stock tooltip. Only when it says it raised
//     nothing (return 0) do we offer the H1.9 card tooltip, so the Dropping page's own item
//     tooltips are untouched.
// -------------------------------------------------------------------------------------------------
int __fastcall CUIMonsterBook_OnMouseMove_hook(void* pThis, void* edx, int rx, int ry) {
    MUSH_FEATURE("monsterBookSearch:mousemove");
    void* pBook = pThis ? reinterpret_cast<char*>(pThis) - 4 : nullptr;
    if (!pBook || pBook != g_st.book) {
        return CUIMonsterBook_OnMouseMove_Orig(pThis, edx, rx, ry);
    }

    // ROUND 11: the heartbeat the tick uses to retire a stuck hover. The engine never tells a UI
    // window that the cursor LEFT it -- `CWndMan` just routes the next move to whatever window is
    // under the pointer (0x009E42B2) -- so "the moves stopped" is the only available signal.
    g_st.lastMoveTick = GetTickCount();

    // K2.3 -- the module-drawn level magnifier gets no hover frame from the engine, so it tracks the
    // cursor itself: `mouseOver/0` inside its rect, `normal/0` outside. Only on the home screen, per
    // the I1.2 screen split, and the repaint is immediate because the window canvas has no dirty
    // flag (EnsureLevelBoxFresh). Edge-triggered, so an unchanged hover costs one PtIn per move.
    {
        const bool overMag =
                OnHomeScreen(pBook) && PtIn(rx, ry, kLvlBtnX, kLvlBtnY, kLvlBtnW, kLvlBtnH);
        if (overMag != g_st.levelMagHover) {
            g_st.levelMagHover = overMag;
            EnsureLevelBoxFresh(pBook);
        }
    }

    if (g_st.itemViewLive) {
        const int slot = HitItemSlot(pBook, rx, ry);
        const int itemId = ItemAtSlot(slot);
        // Logged for BOTH outcomes: "the hover fires but resolves nothing" and "it resolves an item
        // and the tooltip still does not appear" are different bugs, and round 6's log could not
        // tell them apart because it only wrote a line when an item was found.
        ShowItemToolTip(pThis, pBook, rx, ry, itemId);
        g_st.hoverCardMob = 0;
        // Same return contract as the original: 1 when a tooltip was raised (0x00862513), else 0.
        return itemId > 0 ? 1 : 0;
    }

    const int stock = CUIMonsterBook_OnMouseMove_Orig(pThis, edx, rx, ry);
    if (stock != 0) {
        // The client raised its own tooltip on this move; ours must not fight it.
        g_st.hoverCardMob = 0;
        g_st.tipOurs = false;
        return stock;
    }

    // H1.9 -- a card icon.
    if (!OnTierScreen(pBook)) {
        DropOurToolTip(pBook);
        return stock;
    }
    const int slot = HitCardSlot(pBook, rx, ry);
    const int mobId = (slot >= 0) ? GridSlotMobId(pBook, slot) : 0;
    if (mobId <= 0) {
        DropOurToolTip(pBook);
        return stock;
    }
    const std::string name = MobName(mobId);
    if (name.empty()) {
        DropOurToolTip(pBook);
        return stock;
    }
    if (mobId != g_st.hoverCardMob || !g_st.tipOurs) {
        g_st.hoverCardMob = mobId;
    }
    ShowCardToolTip(pThis, pBook, rx, ry, name, MobLevel(mobId));
    return 1;
}

// -------------------------------------------------------------------------------------------------
// Buttons. Both magnifiers land here (a CCtrlButton reaches OnButtonClicked through
// CUIMonsterBook::OnChildNotify, 0x00861F91 -> 0x00861FC8, for any child notification with
// param1 == 100), and so do the four pagers. Nobody else attaches here, so this is a plain detour.
// -------------------------------------------------------------------------------------------------
void __fastcall CUIMonsterBook_OnButtonClicked_hook(void* pThis, void* edx, unsigned int nId) {
    MUSH_FEATURE("monsterBookSearch:button");
    if (!pThis || pThis != g_st.book) {
        CUIMonsterBook_OnButtonClicked(pThis, edx, nId);
        return;
    }

    // I1.9 -- STRICT MAGNIFIER <-> FIELD BINDING.
    //   0x7D0 (bottom, stock)  -> the MOB row's text, and only that
    //   0x7DA (top, ours)      -> the ITEM row's text, and only that
    //   the level magnifier    -> the level SELECTION, and only that (it is not a control at all;
    //                             see HitLevelControl, and 0x7DA is parked off the home screen so
    //                             it can never stand in for it)
    // `RunRowSearch` reads the row through `RowText`, which is the live control for the hosting row
    // and the stash for the other -- never "whatever is typed somewhere".
    if (nId == kNId_BtSearch) { // stock magnifier, BOTTOM row -> MOB scope
        RunRowSearch(pThis, kRowMob);
        // The engine focused the BUTTON on the way in (0x009E3F42); hand the caret back to the row
        // this magnifier belongs to, moving the edit there if it is not already.
        if (g_st.editRow != kRowMob) {
            MoveEditToRow(pThis, kRowMob, true);
        } else {
            FocusCtrl(StockEdit(pThis));
        }
        return; // the stock exact-name jump would fight the substring result set
    }
    if (nId == kNId_BtSearch2) {
        // I1.2 -- this control is the ITEM row's magnifier and NOTHING else. Round 7 let it double
        // as the home screen's level magnifier because both sit at (175,9); the ownership rule
        // forbids one control serving two screens, and the tick now parks it off-screen whenever
        // the book is not on a tier screen. If a click still arrives from somewhere else, it is
        // REFUSED rather than reinterpreted -- a dead click is recoverable, a magnifier that runs
        // the other screen's search is the bug being fixed.
        if (!OnTierScreen(pThis)) {
            return;
        }
        RunRowSearch(pThis, kRowItem);
        if (g_st.editRow != kRowItem) {
            MoveEditToRow(pThis, kRowItem, true);
        } else {
            FocusCtrl(StockEdit(pThis));
        }
        return;
    }
    if (nId == kNId_StockEdit) {
        // I1.10, route ONE of two. `OnChildNotify` forwards any child notification whose param1 is
        // 100 (0x00861FBB) to here, so if this client's CCtrlEdit raises one on Enter it lands
        // here and the search runs on the message that caused it. Route two -- the edge-detected
        // key state on the tick -- exists because nothing in the disassembly PROMISES that it does;
        // the two are made idempotent by `g_st.enterWasDown`, which the tick sets on the same frame
        // it observes the key down, so a doubled Enter re-runs the identical query at worst.
        if (OnTierScreen(pThis)) {
            RunRowSearch(pThis, g_st.editRow);
            FocusCtrl(StockEdit(pThis));
        }
        return;
    }

    // The LEFT-page pagers page the card result set while it is up; and while the ITEM view owns
    // the right page with an empty grid they page IT too, so the player has a working pager on
    // either side of the book.
    if (nId == kNId_CardPrev || nId == kNId_CardNext) {
        const int step = (nId == kNId_CardNext) ? 1 : -1;
        if (g_st.cardView) {
            const int want = g_st.cardPage + step;
            if (want >= 0 && want < CardPageCount()) {
                g_st.cardPage = want;
                SafeWriteInt(pThis, kOff_CardPage, want);
                MarkAllDirty(pThis);
            }
            return;
        }
        if (g_st.itemViewLive) {
            const int want = g_st.itemPage + step;
            if (want >= 0 && want < ItemPageCount()) {
                g_st.itemPage = want;
                BuildHeaderCaption();
                MarkDirty(pThis, 1);
            }
            return;
        }
    }

    // G0.7 -- the RIGHT page's own pagers drive the item view 16 per page while it is up.
    if (g_st.itemViewLive && (nId == kNId_ListPrev || nId == kNId_ListNext)) {
        const int want = g_st.itemPage + ((nId == kNId_ListNext) ? 1 : -1);
        if (want >= 0 && want < ItemPageCount()) {
            g_st.itemPage = want;
            BuildHeaderCaption(); // so the page number is current before the next Draw borrows it
            MarkDirty(pThis, 1);
        }
        return; // the arrows' enable state follows on the next tick (EnforceListArrows)
    }

    CUIMonsterBook_OnButtonClicked(pThis, edx, nId);
}

// pane 0's redraw (Detours; the pane dispatcher at 0x00866ABB is its only caller in the exe). It
// rebuilds the LEFT PAGE canvas and stores it at book+0xB18, so the mob row's box goes on right
// afterwards, at that canvas's local (8,4).
void __fastcall CUIMonsterBook_RedrawPane0_hook(void* pThis, void* edx) {
    MUSH_FEATURE("monsterBookSearch:redraw0");
    CUIMonsterBook_RedrawPane0(pThis, edx);
    if (pThis && pThis == g_st.book) {
        PaintMobRowGuarded(pThis, kPaintWhy_DrawPass);
    }
}

// primary slot 11 -- `push [esp+4]; call CWnd::Draw; ret 4`. CWnd::Draw has just re-blitted
// `backgrnd` into the window canvas and the UI pass is about to walk the child controls, so this is
// the one point where a window-canvas blit lands ON the parchment and UNDER the edit's own text.
//
// ROUND 11 -- THIS IS THE ONLY PLACE THAT MAY PAINT A WINDOW-SURFACE ROW'S INTERIOR WHILE THE EDIT
// IS ON IT, and the reason is the ordering proof in the block above `kPaintWhy_DrawPass`:
//   * `CWnd::Draw` (0x009E0502..0x009E067B) blits ONLY `backgrnd` and walks no children;
//   * `CCtrlEdit::Draw` (0x004CA700) is the SAME virtual slot (CCtrlEdit's primary vtable
//     0x00AF2C98 index 11) and takes its canvas from the PARENT (0x004CA719 -> 0x004C0690
//     `mov ecx,[esi+0x24]`), so if the child pass ran first the parent's opaque background blit
//     would erase every control in the client -- it does not, therefore children draw SECOND;
//   * and the pass is NOT per frame: the shipped log has eighty seconds (00:18:07.567 ->
//     00:19:27.479) of a live, ticking book on the home screen without a single `PAINT level box`,
//     which is rate-limited to one line per three seconds.
// Everything else in this file therefore calls `PaintWindowStripNow`, which defers the interior.
void __fastcall CUIMonsterBook_WndDraw_hook(void* pThis, void* edx, void* pClipRect) {
    MUSH_FEATURE("monsterBookSearch:wnddraw");
    CUIMonsterBook_WndDraw_Orig(pThis, edx, pClipRect);
    if (pThis && pThis == g_st.book) {
        PaintItemRowGuarded(pThis, kPaintWhy_DrawPass);
    }
}

// OnCreate cannot be missed: it is primary vtable slot 3 and the ctor installs the vptr
// (0x00861C06) before CWnd::CreateWnd dispatches it (0x00861C42), so it runs exactly once per book.
// A close DESTROYS the window (BtClose 0x3E8 -> 0x0092C5AE -> CWndMan::CloseUI 0x00A0631B), but
// ZAllocEx recycles by size class so the next book usually lands on the SAME address -- which is
// why "the pointer changed" was never a reliable reopen signal and this hook is.
// Text placement inside the Brass Ledger boxes. The stock CREATEPARAM (0x004C8D5F) leaves the edit's
// glyph inset at x 0 / y -2, i.e. two pixels ABOVE the control, which the flat grey box hid and the
// 2px brass frame does not. CreateCtrl copies those into ctrl+0x38/+0x3C/+0x40 (0x004CA2B0..0x004CA2C5)
// and derives the visible text width ctrl+0x64 = w - 2*insetX (0x004CA2D2..0x004CA2E2). Both field rows
// read these fields (the edit paints row 0; RowText paints row 1 from the same insets; the caret
// follows via CaretInsetY / CaretBaseX), so rewriting them once per book moves text and caret together.
// x 4 lines typed text up with the baked placeholder (box x 5); y 0 centres Arial 12 in the 17px box.
constexpr int kEditTextInsetX = 4;
constexpr int kEditTextInsetY = 0;
constexpr uint32_t kOff_Ctrl_TextWidth = 0x64;

void ApplyEditInsets(void* pEdit) {
    if (!pEdit) {
        return;
    }
    const int w = SafeReadInt(pEdit, kOff_Ctrl_Width);
    if (w <= 2 * kEditTextInsetX || w > 1024) {
        return; // not the control we think it is: keep the stock look
    }
    SafeWriteInt(pEdit, kOff_Ctrl_TextInsetX, kEditTextInsetX);
    SafeWriteInt(pEdit, kOff_Ctrl_TextInsetY, kEditTextInsetY);
    SafeWriteInt(pEdit, kOff_Ctrl_CaretBaseX, kEditTextInsetX);
    SafeWriteInt(pEdit, kOff_Ctrl_TextWidth, w - 2 * kEditTextInsetX);
    SyncEditCaret(pEdit, "insets");
}

void __fastcall CUIMonsterBook_OnCreate_hook(void* pThis, void* edx, void* pData) {
    MUSH_FEATURE("monsterBookSearch:oncreate");
    ResetForClosedBook("OnCreate");
    CUIMonsterBook_OnCreate_Orig(pThis, edx, pData);
    g_st.book = pThis;
    g_st.seenTick = GetTickCount();
    RebaselineTabs(pThis);
    void* pEdit = SafeReadPtr(pThis, kOff_EditCtrl);
    ApplyEditInsets(pEdit);
}

// The other end of the book's life -- primary slot 4, no direct callers anywhere in the exe, so it
// is dispatched purely through the vtable and cannot be missed.
void __fastcall CUIMonsterBook_OnDestroy_hook(void* pThis, void* edx) {
    MUSH_FEATURE("monsterBookSearch:ondestroy");
    ResetForClosedBook("OnDestroy");
    CUIMonsterBook_OnDestroy_Orig(pThis, edx);
}

// =================================================================================================
// reset + flush
// =================================================================================================

// Audited field by field against SearchState. NOTHING here survives a book: the generation bump
// re-arms every WZ probe, the font and the control-creation attempts as well (6.G G0.2). The only
// things deliberately kept are the two leaked ZXStrings and the art-path string (freeing them at
// DLL unload would run after the engine's allocator is gone) and any WZ canvas already resolved
// (WZ art does not change while the client runs).
void ResetForClosedBook(const char* reason) {
    const bool hadSomething = g_st.book != nullptr || g_st.cardView || g_st.itemView
            || g_st.btSearch2 != nullptr || g_st.tabParked || g_st.arrowsForced
            || g_st.levelSel >= 0 || g_st.levelOpen;
    // The window owns and frees its children, and by the time this runs it may already be gone --
    // so nothing here touches a control. The next book builds its own.
    if (g_st.itemViewLive) {
        MonsterBookSearch_SetItemResultView(false);
    }
    // The PRIVATE tooltip is ours and outlives the book, so it is the one tooltip that must be put
    // away by hand. The book's own dies with the book.
    ClearPrivateTip();
    ReleaseTipSlot();
    g_headerSwapped = false;
    if (g_headerHold) {
        try {
            *g_headerHold = ZXString<char>();
        } catch (...) {
        }
    }
    g_headerBuilt.clear();

    g_st = SearchState(); // every per-book field back to its declared default, in one place
    g_cards.clear();
    g_lower.clear();
    g_cardsForBook = nullptr;
    g_mobLevel.clear();
    g_mobName.clear();
    g_itemName.clear();   // I1.3 / I1.7 -- the CItemInfo name cache
    g_dropperPpm.clear(); // I1.6 -- the per-mob percentages of the last droppers view
    ++g_bookGen; // re-arm every generation-keyed probe
}

void FlushPendingQuery() {
    if (!g_st.pendingArmed) {
        return;
    }
    const DWORD now = GetTickCount();
    if (g_st.lastSendTick != 0 && (now - g_st.lastSendTick) < kSendThrottleMs) {
        return;
    }
    if (!InGame() || !SocketConnected()) {
        return;
    }
    void* pSocket = *reinterpret_cast<void**>(kAddr_CClientSocket_Instance);
    if (!pSocket) {
        return;
    }
    g_st.lastSendTick = now;
    g_st.pendingArmed = false;
    try {
        COutPacket packet(kOp_C2S_MonsterBookQuery);
        packet.Encode1(g_st.pendingType);
        if (g_st.pendingType == kQuery_ItemName) {
            packet.EncodeStr(ZXString<char>(g_st.pendingText.c_str()));
        } else {
            packet.Encode4(static_cast<unsigned int>(g_st.pendingItem));
        }
        CClientSocket_SendPacket(pSocket, &packet);
    } catch (...) {
    }
}

void ApplyReply(void* pBook) {
    if (!g_st.replyReady) {
        return;
    }
    g_st.replyReady = false;

    if (g_st.replyType == kQuery_ItemName) {
        if (!g_st.awaitItems) {
            g_st.replyIds.clear();
            g_st.replyPpm.clear();
            return;
        }
        g_st.awaitItems = false;
        std::vector<int> ids = g_st.replyIds;
        const std::string q = g_st.pendingText.empty() ? g_st.itemQuery : g_st.pendingText;
        SetItemResults(std::move(ids), q);
        BuildHeaderCaption();
        if (pBook) {
            MarkAllDirty(pBook);
        }
    } else if (g_st.replyType == kQuery_Droppers) {
        if (!g_st.awaitDroppers) {
            g_st.replyIds.clear();
            g_st.replyPpm.clear();
            return;
        }
        g_st.awaitDroppers = false;
        if (BuildCardIndex(pBook)) {
            std::vector<int> hits;
            hits.reserve(g_st.replyIds.size());
            // I1.6 -- build the ppm table BEFORE opening the view, because OpenCardView clears it
            // through ClearCardView on the way in when a previous view was up.
            std::unordered_map<int, int> ppm;
            const bool havePpm = g_st.replyPpm.size() == g_st.replyIds.size();
            for (size_t i = 0; i < g_st.replyIds.size(); ++i) {
                const int mobId = g_st.replyIds[i];
                const int ci = FindCardByMob(mobId);
                if (ci >= 0) {
                    hits.push_back(ci);
                    if (havePpm && g_st.replyPpm[i] > 0) {
                        ppm[mobId] = g_st.replyPpm[i];
                    }
                }
            }
            const int itemId = g_st.replyItem > 0 ? g_st.replyItem : g_st.pendingItem;
            ClearCardView(pBook, "droppers-replace"); // wipes any previous ppm table first
            // K7(b): `fromDroppers` is passed IN rather than assigned after the call, because it
            // decides the page stride (20, not 25) and OpenCardView pages, logs and -- through K8 --
            // selects the first result before it returns. The item and the ppm table follow
            // immediately and are only ever read by the next frame's paint pass.
            OpenCardView(pBook, std::move(hits), "droppers", /*fromDroppers=*/true);
            g_st.dropperItem = itemId;
            g_dropperPpm.swap(ppm);
        }
    }
    g_st.replyIds.clear();
    g_st.replyPpm.clear();
}

void TickBody() {
    const DWORD now = GetTickCount();
    void* pBook = g_st.book;
    // J-D -- THE IDLE TIMEOUT IS GONE.
    //
    // Round 9's log has `RESET reason=book-gone` seventeen seconds BEFORE the real DESTROY: the RTC
    // dialog froze the pump, no Update reached this module for 750 ms, and the timeout concluded the
    // book had closed and wiped the player's views, stashes, level filter and park bookkeeping while
    // the book was still on screen. Any modal dialog -- or one bad frame on a loaded machine -- does
    // the same. The three real resets (OnCreate, OnDestroy and the book-pointer change adopted in
    // the Update hook) all fired correctly in that same log, so the timeout was never needed and is
    // deleted rather than retuned: a longer timeout is the same bug with a bigger stall.
    //
    // `seenTick` survives as a pure DIAGNOSTIC -- the TICK heartbeat prints how long it has been
    // since the last Update, so a stall is still visible, it just no longer destroys anything.
    if (!pBook) {
        return;
    }

    // --- row 0's magnifier ---------------------------------------------------------------------
    EnsureSecondMagnifier(pBook);

    // --- the screen, read ONCE per tick ---------------------------------------------------------
    // Everything below branches on these three and nothing re-reads the tab control, so a tab that
    // changes mid-tick cannot leave half of this function on one screen and half on the other.
    const int leftTab = LeftTabOrUnknown(pBook);
    const bool onTier = (leftTab >= 0 && leftTab < kTabNoCard);
    const bool onHome = (leftTab == kTabNoCard);

    // --- ROUND 11: the level magnifier's HOVER state has to be able to go OFF ---------------------
    // K2.3 set `levelMagHover` from `OnMouseMove`, which is only dispatched while the cursor is over
    // this window: `CWndMan` re-resolves the target window from the cursor for every mouse message
    // (0x009E42B2) and simply routes moves elsewhere once the pointer leaves, so the book is never
    // told "the cursor left" and the mouseOver art stuck for the rest of the screen. Two retirements,
    // both cheap: the moves stopped arriving, or the book is not on the home screen at all.
    if (g_st.levelMagHover
            && (!onHome || g_st.lastMoveTick == 0 || (now - g_st.lastMoveTick) > 250)) {
        g_st.levelMagHover = false;
        EnsureLevelBoxFresh(pBook);
    }

    // --- J-C: the (48,9) strip, repainted on EVERY home<->tier transition, in BOTH directions ----
    //
    // The strip lives on the WINDOW canvas, which is a retained buffer: CWnd::Draw re-blits
    // `backgrnd` into it and the module paints the box on top, and whatever was painted last stays
    // there until somebody paints over it. So a screen change that does not repaint leaves the OTHER
    // screen's box on screen -- the round-9 report of a white, placeholder-less box on tier screens,
    // which is exactly what the home screen's level box (`itemBox1`, never the placeholder) looks
    // like. Both boxes occupy the SAME 122x17 rect at (48,9) and both blits are opaque, so a plain
    // repaint of the right one fully replaces the wrong one; the only pixels outside that rect
    // either belong to a control that redraws itself (BtSearch2, on tier screens) or are covered by
    // the other screen's own magnifier blit at (175,9), which is the same 34x17 art in the same
    // place. Nothing else has to be erased.
    //
    // Pane 0 is dirtied with the rest, per J-C, so the LEFT PAGE drops any dropdown leftovers.
    {
        const int screenNow = onTier ? 0 : (onHome ? 1 : -1);
        if (screenNow != g_st.stripScreen) {
            g_st.stripScreen = screenNow;
            LogWindowCanvasChoice(pBook, "screen-transition");
            MarkAllDirty(pBook);
            // The window canvas has no dirty flag of its own -- CWnd::Draw is the only thing that
            // touches it and nothing here can schedule it -- so the box goes on NOW, through the
            // same guarded painter the draw hook uses. `screenNow == -1` paints neither, by I1.2.
            // (Round 11: through `PaintWindowStripNow`, so a transition landing on a tier screen
            // while the player is mid-word in the ITEM field defers instead of blanking it.)
            PaintWindowStripNow(pBook, "screen-transition");
        }
    }

    // --- H1.1: the engine decides which row hosts the edit, and refused moves are re-tried --------
    ReconcileEditRow(pBook);

    // --- a tooltip of ours that stopped receiving hovers -----------------------------------------
    // The BOOK's tooltip is cleaned up by the engine when the cursor leaves the window (that is what
    // IUIMsgHandler::ClearToolTip, slot 13, is for). The PRIVATE one is ours alone and has no such
    // owner, so if no OnMouseMove has reached us for a moment it is put away by hand rather than
    // left hanging over the screen.
    if (g_st.tipOurs && g_st.lastHoverTick != 0 && (now - g_st.lastHoverTick) > 700) {
        DropOurToolTip(pBook);
    }

    // The edit belongs to row 1 whenever the book leaves the card screens, so re-opening a colour
    // tab never finds the caret parked on a row the player did not choose. (Not while it is PARKED:
    // the move would drag it back on screen and undo K2.1's vacated rect.)
    if (onHome && !g_st.editParked && g_st.editRow != kRowMob) {
        MoveEditToRow(pBook, kRowMob, false);
    }

    // --- K2.1: the stock edit vacates its rect while the home screen is up ----------------------
    // The level dropdown starts flush under its box now, which means it covers the edit's own rect
    // (49,30)-(169,45) -- and `CWnd::HitTest` (0x009E01E7) routes by position with no regard for the
    // shown flag, so a merely hidden edit would still swallow the first two dropdown rows.
    // `LevelListY()` keys the whole geometry on the read-back, so a park that does not take costs a
    // 20 px gap and never a dead row.
    if (leftTab != kTabUnknown) {
        ParkStockEdit(pBook, onHome);
    }

    // --- G0.4 navigation watcher ----------------------------------------------------------------
    WatchNavigation(pBook);

    // --- G0.5 / G0.7 / I1.4 borrowed chrome, observe-then-correct every tick ---------------------
    //
    // I1.4: the content tab strip is parked for TWO reasons now. The item view is the old one. The
    // HOME screen is the new one -- writing the tab INDEX back to 0 (which monsterBookDrops.cpp's
    // pump does, and which stays as belt and braces) makes the home screen show Basic Info, but it
    // does nothing about the four tabs still being CLICKABLE there, because `CWnd::HitTest` routes
    // by position and `CCtrlWnd::IsHit` never looks at the shown flag. Position is this module's
    // lever and this module's alone, so the strip goes off-screen while the book is on the cover.
    // (kParkTabStripForItemView decides only the ITEM-VIEW half; the home park is I1.4 and stays.)
    const bool wantTabPark = (g_st.itemViewLive && kParkTabStripForItemView) || onHome;
    if (wantTabPark) {
        ParkTabStrip(pBook, g_st.itemViewLive ? "item-view" : "home");
    } else {
        UnparkTabStrip(pBook);
    }
    EnforceListArrows(pBook, g_st.itemViewLive);
    EnforceCardArrows(pBook, g_st.cardView && onTier);

    // --- I1.2: the screen split, re-asserted from scratch every tick ----------------------------
    // BtSearch2 belongs to the tier screens; the level control belongs to the home screen; an
    // unreadable tab gets neither. Parking (not hiding) is what actually removes the button from
    // the home screen -- see ParkSearchButton2. This is also the self-heal for a park that outlived
    // its bookkeeping, because it works off the READ-BACK position rather than off `mag2Parked`.
    if (leftTab != kTabUnknown) {
        ParkSearchButton2(pBook, !onTier);
    }
    // ...and an OPEN DROPDOWN that somehow survived onto a tier screen is rolled up here rather
    // than waiting for the navigation watcher's edge. This is the belt to WatchNavigation's braces:
    // the watcher only fires on a CHANGE it observes, and the in-game bleed (images 1 and 3, the
    // level dropdown drawn over the item field on a tier screen) is precisely the state where that
    // edge was missed.
    //
    // Only `levelOpen` is tested. `levelSel` and `levelView` draw NOTHING off the home screen --
    // PaintLevelControl and PaintLevelDropdown both bail on `!OnHomeScreen` -- and `levelView` is
    // deliberately true ON TIER 0 right after a level search, because that is where
    // RunLevelSearchNow puts the results. Asserting on those two would tear down the search the
    // player just ran, every single time.
    if (onTier && g_st.levelOpen) {
        g_st.levelOpen = false;
        MarkAllDirty(pBook);
    }
    // K2.2 -- and the level box is brought up to date here too, so a pick made on a frame where the
    // Update pump was busy still shows within one tick rather than at the next screen change.
    EnsureLevelBoxFresh(pBook);

    // --- H1.3: the colour strip must stay clickable, whatever else this module borrows ----------
    // Nothing here is supposed to touch book+0xAD8 (RefreshCtrls does not either -- its whole body
    // is SetShow/SetEnable on 0xAF0/0xAF8/0xB00/0xB08/0xB10/0xAE8 only), but "tier switching is
    // impossible while the item view is up" is exactly what a hidden or disabled strip looks like,
    // so it is asserted and any correction is shouted about rather than assumed away.
    {
        void* pLeftTab = SafeReadPtr(pBook, kOff_LeftTabCtrl);
        if (pLeftTab) {
            const bool s = ShowCtrl(pLeftTab, 1);
            const bool e = EnableCtrl(pLeftTab, 1);
        }
    }

    // --- I1.1 / I1.5: the LEFT PAGE only repaints when the client is told to --------------------
    //
    // The MOB row rides the LEFT PAGE canvas, which ONLY Redraw(0) rebuilds, so a change to what
    // that row should show has to raise the client's own pane-0 dirty flag or the new glyphs never
    // reach the screen.
    //
    // ROUND-7 BUG, fixed here, and a leading suspect for "typing blocks the tier click": the old
    // detector compared the LIVE control text against `rowPainted[]`, which is only ever written by
    // a paint pass -- and that pass is skipped whenever the window/page canvas cannot be read or
    // the book is on the wrong screen. Any skip therefore left the two permanently unequal WHILE A
    // ROW HELD TEXT, and the tick raised the dirty flag on EVERY SINGLE FRAME from then on. Each of
    // those frames runs `Redraw(0)`, which builds a fresh `Canvas` object of its own (0x00863E2E).
    // That is an unbounded per-frame cost that starts the moment the player types and never stops,
    // which is exactly the shape of the reported symptom.
    //
    // Two changes make it self-limiting. First, what is COMPARED is now what the module is actually
    // allowed to paint -- the stash for a non-hosting row, the empty string for the hosting one
    // (I1.1) -- so the live text a player is typing does not enter into it at all. Second, the tick
    // remembers the value it has ALREADY dirtied for, so an unchanged desired state raises the flag
    // exactly once no matter how many paint passes are skipped.
    // K3.1/K3.2 make this detector track two more things than it used to, and both are deliberate:
    // the VARIANT is now a function of the row's text (so an emptied hosting row has to repaint to
    // get its placeholder back), and row 1's text is MODULE-owned even while hosting (so every
    // keystroke on that row has to reach the left page). `RowTextToPaint` returns exactly what the
    // paint pass will draw, which keeps the comparison and the paint in lockstep, and the
    // "already dirtied for" pair still bounds it to ONE dirty per actual change.
    if (leftTab != kTabUnknown) {
        bool dirty = false;
        for (int row = 0; row < 2; ++row) {
            const std::string want = RowTextToPaint(pBook, row);
            if (!g_st.rowDirtiedValid[row] || want != g_st.rowDirtiedFor[row]) {
                g_st.rowDirtiedFor[row] = want;
                g_st.rowDirtiedValid[row] = true;
                dirty = true;
            }
        }
        // K3.1: the box VARIANT follows the text now, and row 0's text is the LIVE control while it
        // hosts -- which `RowTextToPaint` deliberately does not report for a window-surface row. So
        // the variant is tracked in its own right, or typing the first letter into the item field
        // would never take its placeholder down.
        for (int row = 0; row < 2; ++row) {
            const int want = BoxVariantForRow(pBook, row);
            if (g_st.rowVariantDirtiedFor[row] != want) {
                g_st.rowVariantDirtiedFor[row] = want;
                dirty = true;
                if (kRowSurface[row] == kSurface_Window) {
                    // No dirty flag reaches the window canvas; ask for it here, this frame.
                    //
                    // ROUND 11 -- THIS LINE IS THE REGRESSION K3.1 INTRODUCED, and it is why it is
                    // now routed through the ordering guard. The variant follows the row's TEXT, so
                    // the FIRST character typed into row 0 lands here -- and this is the tick, not
                    // the draw pass, so a direct blit put an opaque 122x17 `itemBox1` on the
                    // retained window canvas ON TOP of the character the edit had just drawn, with
                    // `drew=""` because row 0's glyphs belong to the control. Nothing repaints that
                    // canvas until the next user-driven pass, so the field read as empty while the
                    // player typed. `PaintWindowStripNow` defers it to the draw hook instead.
                    PaintWindowStripNow(pBook, "variant-change");
                }
            }
        }
        if (g_st.hostDirtiedFor != g_st.editRow) {
            g_st.hostDirtiedFor = g_st.editRow;
            dirty = true;
            PaintWindowStripNow(pBook, "hosting-change");
        }
        // ROUND 11 -- THE CARET HAS TO BLINK, so pane 0 is raised when its phase flips and at no
        // other time. `CCaret::SetVisible` (0x004C932A) divides by 0x12C, so the phase is
        // `((tick - [caret+8]) / 300) & 1` -- at most 3.3 dirty flags a second, and only while the
        // MOB row hosts a SHOWN, FOCUSED edit on a tier screen (the client's own two conditions,
        // 0x004CA22D). Off that state the tracker is parked at -1 and this costs nothing.
        {
            void* pEdit = StockEdit(pBook);
            const bool caretAlive = onTier && ModuleOwnsRowText(g_st.editRow) && pEdit
                    && SafeReadInt(pEdit, kOff_Ctrl_Shown) != 0
                    && FocusOwner() == reinterpret_cast<char*>(pEdit) + 4;
            if (!caretAlive) {
                g_st.caretPhaseDirtiedFor = -1;
            } else {
                const DWORD anchor = static_cast<DWORD>(
                        SafeReadInt(pEdit, kOff_Ctrl_Caret_Obj + kOff_Caret_PhaseTick));
                const int phase = static_cast<int>(((now - anchor) / kCaretBlinkHalfMs) & 1u);
                if (phase != g_st.caretPhaseDirtiedFor) {
                    g_st.caretPhaseDirtiedFor = phase;
                    dirty = true;
                }
            }
        }
        if (dirty) {
            MarkDirty(pBook, 0);
        }
    }

    // --- I1.10: ENTER runs the HOSTING row's search ---------------------------------------------
    // Edge-detected key state, which the spec explicitly allows ("if no clean key route exists" --
    // and there is none: CUIMonsterBook has no OnKey of its own, and OnChildNotify only forwards
    // param1 == 100, which may or may not be what this client's CCtrlEdit raises on Enter).
    // GetAsyncKeyState is global, so all four gates matter:
    //   * this client's window must be in the foreground (otherwise an Enter typed into another
    //     application would fire a search here);
    //   * the book must be on a tier screen (there is no row to search on the cover);
    //   * the EDIT must own the focus (an Enter aimed at the chat box is not aimed at us);
    //   * no dropdown may be open (Enter there means "the list is open", not "search").
    {
        const bool enterDown = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
        if (enterDown && !g_st.enterWasDown) {
            void* pEdit = StockEdit(pBook);
            DWORD fgPid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
            const bool foreground = (fgPid == GetCurrentProcessId());
            const bool focused = pEdit && FocusOwner() == reinterpret_cast<char*>(pEdit) + 4;
            const bool shown = SafeReadInt(pEdit, kOff_Ctrl_Shown) != 0;
            if (foreground && onTier && focused && shown && !g_st.levelOpen) {
                RunRowSearch(pBook, g_st.editRow);
            }
        }
        g_st.enterWasDown = enterDown;
    }

    // --- graceful degradation: no box art -> give the edit its opaque fill back -----------------
    // Decided ONCE PER BOOK (not per session): if this client's UIWindow.img has no mob-row art the
    // transparent field would be invisible parchment, so edit+0x80 goes back to the stock white.
    if (!g_st.editFillRestored && g_box[kRowMob][0].gen == g_bookGen && !g_box[kRowMob][0].canvas) {
        g_st.editFillRestored = true;
        void* pEdit = StockEdit(pBook);
        SafeWriteInt(pEdit, kOff_Ctrl_BackColor, -1);
    }

    // --- deferred work --------------------------------------------------------------------------
    if (g_st.wantMobSearch) {
        g_st.wantMobSearch = false;
        const std::string q = g_st.mobQuery;
        g_st.mobQuery.clear();
        RunMobSearchNow(pBook, q);
    }
    if (g_st.wantLevelSearch) {
        g_st.wantLevelSearch = false;
        RunLevelSearchNow(pBook);
    }
    FlushPendingQuery();
    ApplyReply(pBook);

    if ((g_st.awaitItems || g_st.awaitDroppers) && (now - g_st.awaitSince) >= kAwaitTimeoutMs) {
        g_st.awaitItems = false;
        g_st.awaitDroppers = false;
    }
}

} // namespace

#endif // USE_MONSTER_BOOK_SEARCH

// =================================================================================================
// contract symbols (declared in core/hook.h)
// =================================================================================================

// Main-thread flush (the mod's main-thread flush). Owns every engine call this
// module makes outside its own hook bodies. A fault here is LOGGED and the next tick tries again --
// it never disables the feature (spec 6.G G0.2; the round-5 `g_disabled` latch is what made the
// magnifiers vanish until a game restart).
void MonsterBookSearch_OnClientTick() {
#if USE_MONSTER_BOOK_SEARCH
    MUSH_FEATURE("monsterBookSearch:tick");
    try {
        TickBody();
    } catch (...) {
    }
#endif
}

// S2C 0x372C MONSTER_BOOK_RESULT, routed from features/net/packetDispatcher.cpp.
// NON-CONSUMING, exactly like monsterBookDrops.cpp: the dispatcher only PEEKS the opcode, so the
// cursor still sits on it and every read is bounded with CanRead from offset 0. This ONLY records --
// the view is rebuilt on the flush.
//
//   type 1: [op:2][1][u16 qlen][qlen bytes][u16 n][ n * i32 itemId ]
//   type 2: [op:2][2][i32 itemId]          [u16 n][ n * { i32 mobId, i32 ppm } ]   <- I2, NEW
//
// TYPE 0 IS NOT OURS and is not touched here. It belongs to monsterBookDrops.cpp, whose parser
// opens with `if (p[2] != kResultType_DropTable) return;` (monsterBookDrops.cpp:1272) -- it reads
// the type byte and bails on 1 and 2 before touching a single entry, so widening type 2's rows from
// 4 to 8 bytes cannot disturb it. Verified by reading that file; it is not edited by this round.
//
// The type-2 row width is DETECTED rather than assumed, because the server half of I2 ships in
// parallel: 8 bytes per row when the packet is long enough for it, 4 when it is only long enough
// for the old shape. Getting this wrong in either direction would silently mis-pair mob ids with
// percentages, so the choice is logged every time.
void MonsterBookSearch_OnPacket(CInPacket* pPacket) {
#if USE_MONSTER_BOOK_SEARCH
    MUSH_FEATURE("monsterBookSearch:recv");
    if (!pPacket) {
        return;
    }
    try {
        if (!pPacket->CanRead(3)) {
            return;
        }
        const uint8_t* b = pPacket->Current();
        if (!b) {
            return;
        }
        size_t pos = 2; // the opcode the dispatcher peeked but did not consume
        const int type = b[pos];
        pos += 1;
        if (type != kQuery_ItemName && type != kQuery_Droppers) {
            return;
        }

        int replyItem = 0;
        if (type == kQuery_ItemName) {
            if (!pPacket->CanRead(pos + 2)) {
                return;
            }
            unsigned short qlen = 0;
            memcpy(&qlen, b + pos, sizeof(qlen));
            pos += 2;
            if (!pPacket->CanRead(pos + qlen)) {
                return;
            }
            pos += qlen; // the echoed query is not needed: the flush matches on the awaiting flag
        } else {
            if (!pPacket->CanRead(pos + 4)) {
                return;
            }
            memcpy(&replyItem, b + pos, sizeof(replyItem)); // I1.6 needs to know WHICH item
            pos += 4;
        }

        if (!pPacket->CanRead(pos + 2)) {
            return;
        }
        unsigned short n = 0;
        memcpy(&n, b + pos, sizeof(n));
        pos += 2;
        if (n > kMaxWireRows) {
            return;
        }

        // Row width. Type 1 is unchanged. Type 2 prefers the NEW pair shape and falls back to the
        // old id-only shape only when the packet is too short to be the new one.
        size_t rowLen = kWireRow_ItemsV1;
        bool havePpm = false;
        if (type == kQuery_Droppers) {
            if (pPacket->CanRead(pos + static_cast<size_t>(n) * kWireRow_DroppersV2)) {
                rowLen = kWireRow_DroppersV2;
                havePpm = true;
            } else if (pPacket->CanRead(pos + static_cast<size_t>(n) * kWireRow_DroppersV1)) {
                rowLen = kWireRow_DroppersV1;
            } else {
                return;
            }
        } else if (!pPacket->CanRead(pos + static_cast<size_t>(n) * rowLen)) {
            return;
        }

        std::vector<int> ids;
        std::vector<int> ppm;
        ids.reserve(n);
        if (havePpm) {
            ppm.reserve(n);
        }
        for (unsigned short i = 0; i < n; ++i) {
            int v = 0;
            memcpy(&v, b + pos, sizeof(v));
            int p = 0;
            if (havePpm) {
                memcpy(&p, b + pos + 4, sizeof(p));
            }
            pos += rowLen;
            if (v > 0) {
                ids.push_back(v);
                if (havePpm) {
                    // Kept parallel to `ids` -- a row dropped for a bad id must drop its ppm too.
                    ppm.push_back(p > 0 ? p : 0);
                }
            }
        }

        g_st.replyType = type;
        g_st.replyItem = replyItem;
        g_st.replyIds.swap(ids);
        g_st.replyPpm.swap(ppm);
        g_st.replyReady = true;
    } catch (...) {
    }
#else
    (void)pPacket;
#endif
}

void AttachMonsterBookSearchMod() {
#if USE_MONSTER_BOOK_SEARCH
    MUSH_FEATURE("monsterBookSearch:attach");
    if (MushFeatureQuarantined("monsterBookSearch:update")) {
        // The crash-quarantine system decided a startup crash LOOP is in progress. Nothing is
        // installed this session; the log says so out loud rather than the feature just not existing.
        return;
    }

    // Nobody else attaches to these three, so plain detours are correct (and they get crash
    // attribution for free from ATTACH_HOOK). 0x00863DF1's only caller in the whole exe is the pane
    // dispatcher at 0x00866ABB.
    ATTACH_HOOK(CUIMonsterBook_GetSlotCard, CUIMonsterBook_GetSlotCard_hook);
    ATTACH_HOOK(CUIMonsterBook_OnButtonClicked, CUIMonsterBook_OnButtonClicked_hook);
    ATTACH_HOOK(CUIMonsterBook_RedrawPane0, CUIMonsterBook_RedrawPane0_hook);

    // The stock CCtrlEdit's opaque white fill. CCtrlEdit::Draw skips its DrawRectangle when the
    // colour is 0 (0x004CA784) and the colour comes from CREATEPARAM+0x24, which the book sets with
    // `or dword ptr [ebp-0x48], 0FFFFFFFFh` right after constructing the param (0x00862978).
    // Turning that single instruction into `and dword ptr [ebp-0x48], 0` -- same four bytes -- makes
    // the field transparent so the baked box art underneath shows through, placeholder and all.
    // BYTE-VERIFIED before writing; a client whose bytes differ keeps its opaque edit and says so.
    {
        const unsigned char kExpect[4] = { 0x83, 0x4D, 0xB8, 0xFF };
        const unsigned char kWant[4] = { 0x83, 0x65, 0xB8, 0x00 };
        const unsigned char* p = reinterpret_cast<const unsigned char*>(kAddr_StockEditFill);
        if (memcmp(p, kExpect, sizeof(kExpect)) == 0) {
            PatchMemory(reinterpret_cast<void*>(kAddr_StockEditFill),
                    const_cast<unsigned char*>(kWant), sizeof(kWant));
        }
    }

    // Update (0x00861E5C) and OnMouseButton (0x00862184) already carry another module's detour, so
    // these go in through CUIMonsterBook's OWN vtables instead -- 0x00861E5C appears in .rdata
    // exactly once (0x00B3A034), so no other class is touched, and calling the raw address from a
    // hook body still runs the other module's detour and then the original.
    MushRegisterCode(reinterpret_cast<uintptr_t>(CastHook(&CUIMonsterBook_Update_hook)),
            "CUIMonsterBook_Update_hook");
    MushRegisterCode(reinterpret_cast<uintptr_t>(CastHook(&CUIMonsterBook_OnMouseButton_hook)),
            "CUIMonsterBook_OnMouseButton_hook");
    MushRegisterCode(reinterpret_cast<uintptr_t>(CastHook(&CUIMonsterBook_OnMouseMove_hook)),
            "CUIMonsterBook_OnMouseMove_hook");
    MushRegisterCode(reinterpret_cast<uintptr_t>(CastHook(&CUIMonsterBook_OnCreate_hook)),
            "CUIMonsterBook_OnCreate_hook");
    MushRegisterCode(reinterpret_cast<uintptr_t>(CastHook(&CUIMonsterBook_OnDestroy_hook)),
            "CUIMonsterBook_OnDestroy_hook");
    MushRegisterCode(reinterpret_cast<uintptr_t>(CastHook(&CUIMonsterBook_WndDraw_hook)),
            "CUIMonsterBook_WndDraw_hook");

    // Every vtable install is guarded against the REAL .rdata slot contents, and every outcome --
    // installed, already ours, GUARD FAILED with the bytes found -- goes to the log (G1). A silent
    // skip is how a whole feature quietly stops existing.
    struct VTInstall {
        uintptr_t slot;
        uintptr_t expected;
        void* detour;
        const char* name;
    };
    const VTInstall installs[] = {
        { kVTSlot_Update, kAddr_Update, CastHook(&CUIMonsterBook_Update_hook), "Update(primary[0])" },
        { kVTSlot_OnCreate, kAddr_OnCreate, CastHook(&CUIMonsterBook_OnCreate_hook),
                "OnCreate(primary[3])" },
        { kVTSlot_OnDestroy, kAddr_OnDestroy, CastHook(&CUIMonsterBook_OnDestroy_hook),
                "OnDestroy(primary[4])" },
        { kVTSlot_WndDraw, kAddr_WndDrawThunk, CastHook(&CUIMonsterBook_WndDraw_hook),
                "CWnd::Draw(primary[11])" },
        { kVTSlot_OnMouseButton, kAddr_OnMouseButton, CastHook(&CUIMonsterBook_OnMouseButton_hook),
                "OnMouseButton(IUIMsgHandler[2])" },
        { kVTSlot_OnMouseMove, kAddr_OnMouseMove, CastHook(&CUIMonsterBook_OnMouseMove_hook),
                "OnMouseMove(IUIMsgHandler[3])" },
    };
    for (const VTInstall& v : installs) {
        const uintptr_t have = *reinterpret_cast<uintptr_t*>(v.slot);
        if (have == v.expected) {
            Patch4(v.slot, static_cast<unsigned int>(reinterpret_cast<uintptr_t>(v.detour)));
        }
    }
#endif
}
