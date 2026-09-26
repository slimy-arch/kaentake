// ============================================================
// cashshopwnd.cpp — the standalone Cash Shop window
//
// Replaces ENTERING the cash shop. In v83 the cash shop is a STAGE, not a window:
// its backgrounds are 800x600 (UI/CashShop.img/Base/backgrnd), SET_CASH_SHOP (0x7F)
// falls outside the [0x1D,0x7C] range CClientSocket::ProcessPacket routes to
// CWvsContext and so is dispatched to CurrentStage->OnPacket, and the server detaches
// the player from the channel and map to get there. So there is no stock cash-shop
// UI object to instantiate as a child window, and this is drawn from scratch --
// exactly like CBeautyShop, CUIBagWindow and CUIColorPrism already are.
//
// WHAT IT SHOWS
//   * the real cash shop taxonomy from Etc.wz/Category.img: 7 tabs, 30 categories.
//     Tab and category come off the serial number the same way the server already
//     reads it (World.java:1386 buckets by sn/10000000): tab = sn/10000000,
//     category = (sn/100000)%100. Verified against Etc.wz/Commodity.img -- every one
//     of its 2010 on-sale entries lands in a bucket Category.img declares.
//   * a live avatar preview of the selected item, so cash equips can be tried on.
//   * NX balance and a Buy button.
//
// The catalog is SERVER-SENT (LP 0x3731). Nothing here reads Commodity.img -- the
// window owns its own tabs, so the client's sn-decodes-the-tab convention does not
// bind it; the taxonomy table below is static WZ data copied in rather than parsed.
//
// HOOKS: none. This file Detours nothing.
//   * the inbound opcode is routed by clientsocket.cpp's ProcessPacket dispatcher (a second
//     Detour on 0x004965F1 is the documented way to make replies stop decoding);
//   * the window is opened by the server, which answers the status bar's Cash Shop
//     button (CP 0x28 ENTER_CASHSHOP) with LP 0x3731 resp OPEN instead of doing the
//     stage change. That is why no client hook is needed for the button at all.
//
// THREADING: CashShopWnd_HandleSync runs on the RECEIVE thread. It only fills
// the catalogue under g_mtx and raises g_bWantOpen; the window is created from
// CashShopWnd_Tick on the main thread (there is a reason a
// server push must not build a window where it lands).
//
// -------------------------------------------------------------------------
// WARNING — CLIENT-BUILD-SPECIFIC ADDRESSES
// Every 0x00XXXXXX below is tied to THIS v83 MapleStory.exe (image base 0x400000).
// Every one was re-verified for claude-maple against v83.idb + MapleStory.exe disassembly,
// named from v95.pdb; the v95 name and v83 VA are beside each.
// -------------------------------------------------------------------------
// ============================================================

#include "pch.h"
#include "hook.h"
#include "debug.h"
#include "cashshopwnd.h"
#include "storagebag.h"
#include "clientsocket.h"

#include "wvs/iteminfo.h"
#include "wvs/packet.h"
#include "wvs/util.h"
#include "wvs/wnd.h"
#include "wvs/wndman.h"
#include "ztl/ztl.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace CashShopWnd {

// =====================================================
// PROTOCOL
// =====================================================
constexpr unsigned char kAction_RequestCatalog  = 0;
constexpr unsigned char kAction_Buy             = 1;
constexpr unsigned char kAction_BuyCart         = 2;   // + count, count * itemId
constexpr unsigned char kAction_RequestCategory = 3;   // + tab, category

constexpr unsigned char kResp_Open    = 0;   // + 3 ints of cash
constexpr unsigned char kResp_Catalog = 1;   // + flag, tab, cat, count, entries
constexpr unsigned char kResp_Buy     = 2;   // + code, itemId
constexpr unsigned char kResp_Cash    = 3;   // + 3 ints of cash
constexpr unsigned char kResp_BuyCart = 4;   // + code, itemId, delivered, spent
constexpr unsigned char kResp_Index   = 5;   // + count, count * (tab, cat, int n)

// Indexed by the server's BUY_* codes. Kept bounds-guarded at every use so a server that
// grows a new code degrades to "Purchase failed." on an un-updated DLL rather than
// reading past the end.
const char* const kBuyMsg[] = {
    "Purchased.",
    "Not enough NX.",
    "Unknown item.",
    "Your inventory is full.",
    "That item is not on sale.",
    "Another purchase is already in progress.",
    "That cart is not valid.",
};

// Sanity ceilings. A truncated or hostile packet must be rejected whole, the way
// a sibling window guards its catalog, rather than half-applied.
//
// kMaxCatalog is now a PER-CATEGORY guard, not a total. The window used to be handed the
// whole catalogue on open and hold it in one list, which made 16384 a hard ceiling on the
// shop -- and the failure was silent, because the chunk that would have crossed it simply
// returned and the rest of the merchandise vanished with no error anywhere. Categories are
// fetched on demand now, so the only thing that has to fit is the one being looked at.
constexpr int kMaxEntriesPerChunk = 2048;
constexpr int kMaxPerCategory     = 65536;
constexpr int kMaxNameLen         = 64;

// =====================================================
// ADDRESSES (all shared with an existing feature)
// =====================================================
// ---- Esc: the client's close stack ------------------------------------------
// CWvsContext::ProcessBasicUIKey on Esc (0x00A074A5) closes GetTopStackForTab() through
// TryCloseUI, and opens the game menu (UI_Menu 0x00A06735) when that returns 0. Stock
// windows join the stack in OnCreate and leave it in OnDestroy; TryCloseUI only knows the
// 19 stock window types, so this window is recognised by the hook in AttachCashShopWndMod.
constexpr uintptr_t kAddr_PushStackForTab       = 0x0091BE95;  // CWvsContext::PushStackForTab — v95 sym, v83 VA 0x0091BE95 (ret 4)
constexpr uintptr_t kAddr_RemoveFromStackForTab = 0x007FE499;  // CWvsContext::RemoveFromStackForTab — v95 sym, v83 VA 0x007FE499 (ret 4; absent entry is a no-op)
constexpr uintptr_t kAddr_GetTopStackForTab     = 0x00A07599;  // CWvsContext::GetTopStackForTab — v95 sym, v83 VA 0x00A07599 (ret 0)
constexpr uintptr_t kAddr_TryCloseUI            = 0x00A06E55;  // CWvsContext::TryCloseUI — v95 sym, v83 VA 0x00A06E55 (ret 4)

// ---- the stock item tooltip -------------------------------------------------
// Stock windows (CUIItem::OnMouseMove 0x0081D8AD, CAdminShopDlg::OnMouseMove 0x00425E48)
// call ShowItemToolTip on EVERY mouse move, which is what makes it follow the cursor, and
// ClearToolTip when nothing is hovered. Each window owns its CUIToolTip (0x514 bytes).
constexpr uintptr_t kAddr_ToolTip_Ctor          = 0x008E49B5;  // CUIToolTip::CUIToolTip — v95 sym, v83 VA 0x008E49B5
constexpr uintptr_t kAddr_ToolTip_Dtor          = 0x008E6BA3;  // CUIToolTip::~CUIToolTip — v95 sym, v83 VA 0x008E6BA3
constexpr uintptr_t kAddr_ToolTip_Clear         = 0x008E6E23;  // CUIToolTip::ClearToolTip — v95 sym, v83 VA 0x008E6E23 (ret 0)
constexpr uintptr_t kAddr_ShowItemToolTip       = 0x008F5B20;  // CUIToolTip::ShowItemToolTip — v95 sym, v83 VA 0x008F5B20 (ret 0x20)
constexpr uintptr_t kAddr_ItemInfo_GetItemSlot  = 0x005D5D95;  // CItemInfo::GetItemSlot — v95 sym, v83 VA 0x005D5D95 (ret 8)
constexpr uintptr_t kAddr_ZRefItemSlot_Release  = 0x00428A50;  // ZRef<GW_ItemSlotBase>::~ZRef, v83 VA 0x00428A50 (ret 4; does NOT null-check p)
constexpr uintptr_t kAddr_CItemInfo_Instance    = 0x00BE78D8;  // TSingleton<CItemInfo>::ms_pInstance
// Sending goes through clientsocket.cpp's SendClientPacket (CClientSocket::SendPacket 0x0049637B).
constexpr uintptr_t kAddr_play_ui_sound           = 0x00989588;  // play_ui_sound — v95 sym, v83 VA 0x00989588 (cdecl)
constexpr uintptr_t kAddr_SetFont                 = 0x0046341A;  // IWzFont::Create wrapper, v83 VA 0x0046341A (ret 0x10; THROWS on failure)
constexpr uintptr_t kAddr_ProcessBasicUIKey       = 0x00A07431;  // CWvsContext::ProcessBasicUIKey — v95 sym, v83 VA 0x00A07431 (ret 8; Detoured by damagerankinput.cpp)
constexpr uintptr_t kAddr_CWvsContext             = 0x00BE7918;  // TSingleton<CWvsContext>::ms_pInstance, v83 0x00BE7918
constexpr uintptr_t kAddr_CurrentStage            = 0x00BEDED4;  // ZRef<CStage> g_pStage (0x00BEDED0) + 4 = p
constexpr uintptr_t kAddr_InputSystem             = 0x00BEC33C;  // TSingleton<CInputSystem>::ms_pInstance, v83 0x00BEC33C
constexpr uintptr_t kAddr_GetCursorPos            = 0x0059A388;  // CInputSystem::GetCursorPos — v95 sym, v83 VA 0x0059A388 (ret 4, returns int)

// The avatar-preview quartet, all byte-verified.
// ZRef<CAvatar>::_Alloc -> 0x00428EF0 -> 0x0042A406 calls the CAvatar ctor 0x0044FE6C, which
// avatar.cpp hooks, so the preview avatar gets the CustomData its patched Update reads at +0x484.
constexpr uintptr_t kAddr_ZRefCAvatar_Alloc = 0x00428967;  // ZRef<CAvatar>::_Alloc — v95 sym, v83 VA 0x00428967 (ret 0)
constexpr uintptr_t kAddr_ZRef_Release      = 0x00428C15;  // ZRef<CAvatar> release, v83 VA 0x00428C15 (ret 4; does not null the slot)
constexpr uintptr_t kAddr_AvatarLook_ctor   = 0x004283FE;  // AvatarLook::AvatarLook(const CharacterData&) — v95 sym, v83 VA 0x004283FE (ret 4)
constexpr uintptr_t kAddr_CAvatar_Init      = 0x0045149F;  // CAvatar::Init — v95 sym, v83 VA 0x0045149F (ret 0x24, 9 args)
constexpr int       kOff_CharacterDataInCtx = 2094 * 4;    // CWvsContext::GetCharacterData reads [ecx+0x20B8]
constexpr int       kAvatarScale            = 100;

// AvatarLook field offsets, from a sibling window.
constexpr int kAL_WeaponSticker = 0x15;
constexpr int kAL_HairEquip0    = 0x19;   // anHairEquip[0]; slot n is +4*n
constexpr int kAL_Face          = 0x11;
constexpr int kAL_Hair          = 0x19;   // == anHairEquip[0], which is the hair slot

// ---- the playground avatar -------------------------------------------------
// CAvatar+0x4E8 (m_nMoveAction) is a PACKED MOVE ACTION, not an action code:
//     nMoveAction = (moveAction << 1) | facingBit,  facingBit 1 = LEFT.
// Byte-verified in the client's own decoder at 0x00451EC8, which does
// `and edx,1` into the pnDir out-param and `sar eax,1` for the move action. The
// client ships the matching composer, so it is called rather than hand-shifted.
//   int __cdecl MakeMoveAction(int nMoveAction, int bFlip) -> (nMoveAction<<1)|(bFlip&1)
//
// THE MOVE ACTION NAMESPACE IS ONLY 1..10, which is what makes an action code the
// wrong thing to pass here. With [+0x4A0]==0 and [+0x4B4]==0, 0x00451FB6 does
// `cmp eax,9; ja` on (moveAction-1) and dispatches through the table at 0x0045207C;
// anything out of range falls through to 0x00451FFD `xor eax,eax` = action code 0 = walk1.
// (Other flag states use the tables at 0x00452004/0x0045202C/0x00452054 instead.) Reading each handler off that
// table gives the whole mapping, and it is a fixed client enum rather than WZ data:
//     1  -> walk1/walk2 (0/1)   2 -> stand1/stand2 (2/3)   3 -> jump   (35)
//     4  -> alert       (4)     5 -> prone         (33)    6 -> fly    (34)
//     7  -> ladder      (36)    8 -> rope          (37)    9 -> dead   (38)
//     10 -> sit         (39)
// walk2 / stand2 are the client's own choice from CAvatar+0x468 / +0x46C, not ours.
// This is also why beautyshop.cpp's literal 5 is not "action 5": it is
// MakeMoveAction(2, 1) = move action 2 (stand) with the facing bit set, exactly what
// both of the client's own CAvatar::Init call sites push (0x004265A6 `push 1; push 2;
// call 0x426626`).
//
// Poses OUTSIDE 1..10 -- here, the attack swing -- are unreachable as move actions and
// are struck through the ACTION CODE OVERRIDE at CAvatar+0x4EC instead, which
// CAvatar::GetCurrentAction (v95 name; 0x00451E4C) prefers over the move action whenever it is > -1:
//     mov eax,[esi+0x4E8]; call 0x451EC8   ; move action -> code, kept in edi
//     mov ecx,esi; call 0x451B6A           ; read the override
//     cmp eax,-1; jle -> use edi           ; override wins unless it is -1
// The setter is CAvatar::SetOneTimeAction (v95 name) @0x004571AB, __thiscall, one int arg (`ret 4`):
//     or [esi+0x4EC],-1  /  ClearActionLayer(1)  /  mov [esi+0x4EC],arg
//     /  call [vtbl+0x14] = PrepareActionLayer(6,100,0)
// It rebuilds the layers itself, so nothing else is needed -- no rebuild, no Update tick.
constexpr uintptr_t kAddr_MakeMoveAction        = 0x00426626;  // (a<<1)|(b&1), cdecl, v83 VA 0x00426626
constexpr uintptr_t kAddr_CAvatar_SetMoveAction = 0x004520F1;  // CAvatar::SetMoveAction — v95 sym, v83 VA 0x004520F1 (ret 8)
constexpr uintptr_t kAddr_CAvatar_SetActionCode = 0x004571AB;  // CAvatar::SetOneTimeAction — v95 sym, v83 VA 0x004571AB (ret 4)
constexpr uintptr_t kAddr_GetActionNameFromCode = 0x004A8CE6;  // get_action_name_from_code — v95 sym, v83 VA 0x004A8CE6 (itemeff.cpp)

// CAvatar+0x10AC is the avatar's POSITION: a raw IWzVector2D* that CAvatar::Init
// creates with put_origin(the layer you passed) + RelMove(x, y). It is written
// exactly twice in the whole image (null in the ctor, assigned in Init) and only
// ever read afterwards, so driving it does not fight the client.
// NOTE this is NOT wvs/avatar.h's m_pBodyOrigin (0x10B8) -- that is a different
// vector in the same cluster and moving it does not move the sprite.
constexpr size_t kOff_AvatarPosVector = 0x10AC;

using t_MakeMoveAction  = int(__cdecl*)(int, int);
using t_SetMoveAction   = void(__thiscall*)(void*, int, int);
using t_SetActionCode   = void(__thiscall*)(void*, int);
// CODE -> NAME. The out param is caller-owned, which is what makes it safe to call.
using t_GetActionName   = Ztl_bstr_t*(__cdecl*)(Ztl_bstr_t*, int);

auto MakeMoveAction        = reinterpret_cast<t_MakeMoveAction>(kAddr_MakeMoveAction);
auto CAvatar_SetMoveAct    = reinterpret_cast<t_SetMoveAction>(kAddr_CAvatar_SetMoveAction);
auto CAvatar_SetActionCode = reinterpret_cast<t_SetActionCode>(kAddr_CAvatar_SetActionCode);
auto GetActionNameFromCode = reinterpret_cast<t_GetActionName>(kAddr_GetActionNameFromCode);

inline int PackMoveAction(int moveAction, bool bFacingLeft) {
    return MakeMoveAction(moveAction, bFacingLeft ? 1 : 0);
}

// The poses the playground can strike. moveAction is the fixed 1..10 client enum
// above; actionCode is the CAvatar+0x4EC override, needed only by a pose that has no
// move action of its own.
constexpr int kNoActionCode = -1;
struct Pose { int moveAction; int actionCode; };
constexpr Pose kPoseStand { 2, kNoActionCode };
constexpr Pose kPoseWalk  { 1, kNoActionCode };
constexpr Pose kPoseJump  { 3, kNoActionCode };
constexpr Pose kPoseProne { 5, kNoActionCode };
// Move action 7. The client draws a climbing avatar FROM BEHIND, which is the entire reason
// the cash shop paints a ladder into its backdrop: it is how you see the back of a cape.
constexpr Pose kPoseLadder { 7, kNoActionCode };

// Action codes index the client's 162-entry action table at 0x00BEC620 (stride 0x18,
// running to 0x00BED550; 0xF30/0x18 == 162 exactly). That table is NOT built from
// Character/00002000.img -- it is a CRT static initializer (registered at 0x00BD7188,
// body 0x004A38E7..0x004A60E3) that pulls hardcoded ids out of the executable's own
// encrypted string pool. Its order is therefore baked into this binary and no WZ
// import can perturb it, which is what makes hardcoding a code safe. Confirmed order
// includes 0=walk1 1=walk2 2=stand1 3=stand2 4=alert 5=swingO1 16=stabO1 22=shoot1
// 31=heal 32=proneStab 33=prone 34=fly 35=jump 36=ladder 37=rope 38=dead 39=sit --
// every one of the move-action codes in that list is independently confirmed by the
// 0x0045207C dispatch above.
constexpr int kActionTableCount   = 162;
constexpr int kActionCode_SwingO1 = 5;

// The live swing code, checked once at startup so a different client build says so in
// the log instead of silently striking an unrelated animation. kNoActionCode = this
// build has no swingO1 and the attack pose is simply unavailable.
int  g_nSwingCode  = kActionCode_SwingO1;
bool g_bSwingChecked = false;

// The swing has no move action of its own, so it is a stand base plus the override.
// It degrades to a plain stand on a build where the code did not resolve.
inline Pose PoseAttack() { return Pose{ kPoseStand.moveAction, g_nSwingCode }; }

// ---- THE FRAME DRIVER ------------------------------------------------------
// void __thiscall CAvatar::Update(void)   0x004522A6   (bare `ret`, zero stack args)
//
// An avatar's body frames are NOT animated by the Gr2D engine. PrepareActionLayer
// (vtbl+0x14, 0x00453AD1) only InsertCanvas'es an action's frames into the body layers
// and never calls IWzGr2DLayer::Animate on them -- so a freshly prepared layer sits on
// canvas index 0 and stays there. This routine is what steps it:
//   * 0x0045275E `add dword ptr [ecx+ebx+0x4F4], -0x1E`  the frame countdown, and note
//     it is a FIXED 30 PER CALL rather than elapsed time;
//   * 0x00452805 `inc dword ptr [esi]` (esi = CAvatar+0x4F0, or +0xACC while the +0x4EC
//     override is live) the frame index, then ShiftCanvas onto the body layers;
//   * 0x00452CD2 `add dword ptr [eax+4], edx` re-arms the countdown from the NEW frame's
//     own WZ delay, read out of the int table at layer+0x0C.
// Because the step is a fixed 30 per call, the cadence is "once per Update()" and must
// NOT be scaled by dt -- that is exactly what the client does: CUser::Update calls it at
// 0x00931C5D (`lea ecx,[ebx+0x88]; call 0x4522A6`), and two stock CWnd Update overrides
// (0x009010BA, 0x00605918) do the same for their own preview avatars.
//
// It is not in the CAvatar vtable -- that has only 6 slots (0x00AF14D8) -- and a sweep of
// .rdata/.data for the literal 0x004522A6 finds no hits at all, so nothing this DLL
// already calls reaches it. beautyshop.cpp and coloringprism.cpp have the same gap and
// their previews are frozen too.
constexpr uintptr_t kAddr_CAvatar_Update = 0x004522A6;  // CAvatar::Update — v95 sym, v83 VA 0x004522A6 (ret 0)
using t_CAvatar_Update = void(__thiscall*)(void*);
auto CAvatar_Update = reinterpret_cast<t_CAvatar_Update>(kAddr_CAvatar_Update);

// ---- FACIAL EXPRESSIONS ----------------------------------------------------
// void __thiscall CAvatar::SetEmotion(CAvatar*, int nEmotion, int tDurationMs)  0x00451CF6
// (`ret 8`, not virtual -- the vtable is only 6 slots wide).
//
// nEmotion is an INDEX into the fixed 23-entry table at 0x00BEC284, gated by SetEmotion's
// own `cmp edi,0x17 / jge` at 0x00451D4F. The order is a CRT static initializer, so it is
// baked into this binary:
//   0 blink  1 hit  2 smile  3 troubled  4 cry  5 angry  6 bewildered  7 stunned
//   8 vomit  9 oops 10 cheers 11 chu 12 wink 13 pain 14 glitter 15 blaze 16 shine
//   17 love 18 despair 19 hum 20 bowing 21 hot 22 dam
// That order is NOT the WZ node order -- Character/Face/*.img lists glitter, despair,
// love, shine, blaze where the client has glitter, blaze, shine, love, despair -- so
// deriving the index by enumerating the .img children gets four of them wrong.
//
// tDurationMs really is milliseconds: 0x0045393A stores now + duration into CAvatar+0x48C,
// and CAvatar::Update compares against it at 0x004534A5 and calls SetEmotion(0,-1) itself
// at 0x004534B3 when it passes. So the five-second hold is the CLIENT'S timer, not ours --
// but only because this window now ticks the avatar. A negative duration means "use the
// emotion's own WZ length" (whose fallback default, 0x00407D3D, is 5000 anyway).
constexpr uintptr_t kAddr_CAvatar_SetEmotion = 0x00451CF6;  // CAvatar::SetEmotion — v95 sym, v83 VA 0x00451CF6 (ret 8)
using t_SetEmotion = void(__thiscall*)(void*, int, int);
auto CAvatar_SetEmotion = reinterpret_cast<t_SetEmotion>(kAddr_CAvatar_SetEmotion);

// TSingleton<CAnimationDisplayer>::ms_pInstance -- NOT g_pField (there is no such global in v83;
// get_field 0x00437A0C reads the stage). SetEmotion null-tests it at 0x00451DD4 before calling
// CAnimationDisplayer::Effect_General (0x00439750). See DisplayerHide for why this file cares.
constexpr uintptr_t kAddr_AnimationDisplayer = 0x00BEBF6C;

constexpr int kEmotionCount     = 23;
constexpr int kNoEmotion        = -1;
constexpr int kExpressionHoldMs = 5000;

// Etc.wz/Commodity.img tab 5 sub 3 references exactly these 15 ids. 0516.img holds 40
// nodes, but the ids above 5160014 map to index >= 23 and SetEmotion's own range gate
// rejects them -- the stock client cannot play them either, so this bound is not a
// limitation this window is imposing.
constexpr int kExprItemFirst = 5160000;
constexpr int kExprItemLast  = 5160014;

// THE MAPPING, lifted from the client's own arithmetic at 0x004AD12F:
//     mov eax,[esp+4] / push -1 / push 100 / cdq / pop esi / idiv esi
//     add edx,8 / push edx / call 0x00451CF6
// i.e. nEmotion = (itemId % 100) + 8. Item.wz/Cash/0516.img carries NO emotion metadata
// (just info/{icon, iconRaw, cash}) -- there is nothing in the WZ to read.
inline int EmotionOfCashItem(int itemId) {
    if (itemId < kExprItemFirst || itemId > kExprItemLast) return kNoEmotion;
    const int n = (itemId % 100) + 8;
    return (n >= 0 && n < kEmotionCount) ? n : kNoEmotion;
}

// ---- CASH "EFFECT" ITEMS ---------------------------------------------------
// void** __cdecl CreateAnimLayer(void** ppRet, IWzProperty* pNode, int bFlip,
//                                IWzVector2D* pOrigin, int x, int y,
//                                IWzGr2DLayer* pOverlay, int nZ, int nAlpha, int n)
//   0x0043EA3E. Every effect in this client bottoms out here: it turns a WZ animation node
//   into a Gr2D layer and anchors it with put_origin / put_overlay / put_z. Effects are NOT
//   stage-only -- there is one IWzGr2D device in the process (the global at 0x00BF14EC) and
//   CWnd::CreateWnd makes the window's own layer from it. Nesting the effect into the
//   AVATAR'S overlay layer (CAvatar+0x10C8) is therefore all it takes to composite it
//   inside this window: the same mechanism already puts the preview avatar there, since
//   CAvatar::Init's 4th argument becomes that layer's overlay (0x00450A3E). It also means
//   the effect follows the avatar for free -- it is anchored to the avatar's own origin
//   vector, so MoveAvatar drags it along.
//
// OWNERSHIP IS ASYMMETRIC and getting it wrong leaks or double-frees: the callee RELEASES
// pNode, pOrigin and pOverlay (0x0043EEB5 / EEC6 / EED7), so each needs its own AddRef
// first -- the same convention CAvatar::Init uses for its two layer arguments. The out slot
// receives ONE owned reference and is overwritten WITHOUT releasing what was there, so it
// must be zeroed before every call.
constexpr uintptr_t kAddr_CreateAnimLayer = 0x0043EA3E;  // anim-layer loader, v83 VA 0x0043EA3E (cdecl, 10 args)
using t_CreateAnimLayer = void**(__cdecl*)(void**, void*, int, void*, int, int,
                                           void*, int, int, int);
auto CreateAnimLayer = reinterpret_cast<t_CreateAnimLayer>(kAddr_CreateAnimLayer);

constexpr size_t kOff_AvatarFaceOrigin     = 0x10B4;   // CAvatar::GetFaceOrigin 0x00932CBF
constexpr size_t kOff_AvatarBodyOrigin     = 0x10B8;   // == wvs/avatar.h m_pBodyOrigin
constexpr size_t kOff_AvatarLayerUnderFace = 0x10C8;   // CAvatar::GetLayerUnderFace 0x00451E7E

// Commodity tab 5 sub 5 ("Effect"). 5010xxx is the bulk; 5281000/5281001 keep their
// canvases directly under `effect` in a different group img. 5110000 is deliberately NOT
// here -- its art lives in Effect.wz/ItemEff.img, not under Item.wz, and nothing below
// would find it.
inline bool IsCashEffectItem(int itemId) {
    return (itemId >= 5010000 && itemId <= 5019999)
        || (itemId >= 5281000 && itemId <= 5281999);
}

// Which action node a per-action effect should use for the pose currently being struck.
// nPackedMA is (moveAction << 1) | facing, so the move action is the top bits.
const wchar_t* ActionNameForPose(int nPackedMA, int nActionCode) {
    if (nActionCode != kNoActionCode) return L"swingO1";   // the only override struck here
    switch (nPackedMA >= 0 ? (nPackedMA >> 1) : 0) {
        case 1:  return L"walk1";
        case 3:  return L"jump";
        case 5:  return L"prone";
        default: return L"stand1";
    }
}

// Split out purely because __try may not share a function with anything that unwinds, and
// the caller has to hold COM pointers.
void* SehCallCreateAnimLayer(void* pNode, int bFlip, void* pOrigin, void* pOverlay) {
    void* pLayer = nullptr;
    __try {
        CreateAnimLayer(&pLayer, pNode, bFlip, pOrigin, 0, 0, pOverlay, 3, 255, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { pLayer = nullptr; }
    return pLayer;
}

// CAvatar+0x4EC, the action-code override, read back rather than remembered.
// CAvatar::Update ENDS a one-shot action itself: past the last frame it does
// 0x00452817 `or dword ptr [ebx+0x4EC], -1`, ClearActionLayer(1) and
// PrepareActionLayer(6,100,0). So the swing stops on its own and the window must not
// hold its own timer over the top of it -- see StartAttack.
constexpr size_t kOff_ActionCodeOverride = 0x4EC;

// Resolution runs CODE -> NAME, never the reverse.
//
// Deliberately NOT the reverse lookup at 0x004A8D14, which is wrong twice. It compares
// WIDE strings -- the routine it reaches does `mov edx,[eax-4]; shr edx,1` at
// 0x00402F0E (a byte length halved into a wchar count) and `mov bx, word ptr [eax]` at
// 0x00402F22 -- so handing it a ZXString<char> makes it read a length from before a
// narrow buffer and walk ASCII as UTF-16. And it CONSUMES its argument: 0x004A8D41
// calls the ZXString release routine on the argument's own buffer, so a temporary
// passed by value is released a second time by its own destructor. Passing "stand1"
// crashed the client with an access violation reading 0x6E61746F, which is the bytes
// of that name misread.
bool ActionCodeNamed(int code, const wchar_t* want) {
    // 0x004A8CE6 indexes the table with no bounds check of its own (`lea eax,[eax+eax*2];
    // mov eax,[eax*8+0xBEC620]`), so the range is guarded here.
    if (code < 0 || code >= kActionTableCount || !want) return false;
    try {
        Ztl_bstr_t name;                       // fresh per call: the converter overwrites
        GetActionNameFromCode(&name, code);    // the out param without releasing it first
        const wchar_t* got = name.GetBSTR();
        return got && _wcsicmp(got, want) == 0;
    } catch (...) { return false; }
}

int FindActionCode(const wchar_t* want) {
    for (int c = 0; c < kActionTableCount; ++c)
        if (ActionCodeNamed(c, want)) return c;
    return kNoActionCode;
}

void VerifySwingCode() {
    if (g_bSwingChecked) return;
    g_bSwingChecked = true;

    if (ActionCodeNamed(kActionCode_SwingO1, L"swingO1")) return;
    g_nSwingCode = FindActionCode(L"swingO1");
    LOG_ONCE("cashshopwnd: action code %d is not 'swingO1' on this client; %s",
             kActionCode_SwingO1,
             (g_nSwingCode >= 0) ? "rescanned the action table for it"
                                 : "the attack pose is disabled");
}

// Movement. These are the CLIENT'S OWN numbers, read out of Map.wz/Physics.img, so the
// playground avatar moves at exactly 100% base speed and jump rather than at an invented
// display pace:
//     walkSpeed 125.0   jumpSpeed 555.0   gravityAcc 2000.0   fallSpeed 670.0
// (Server/wz/Map.wz/Physics.img.xml). A real character's speed/jump stats scale these;
// 100% is the unscaled value, which is what a preview should show.
constexpr float kWalkPxPerSec = 125.0f;
constexpr float kJumpVel0     = -555.0f;   // px/sec, upward
constexpr float kGravity      = 2000.0f;   // px/sec^2
constexpr float kFallSpeedMax = 670.0f;    // terminal velocity, same source

// MOMENTUM. Physics.img states walking as FORCES -- walkForce 140000, walkDrag 80000 --
// beside gravity as an ACCELERATION, 2000. They are the same units divided by the
// character's mass, and that mass is 100: dividing every force in the file by it lands them
// all in gravity's range and nowhere else does the file make sense (slipForce -> 600,
// swimForce and flyForce -> 1200, floatDrag1 -> 1000, walkForce -> 1400, walkDrag -> 800).
//
// So the avatar accelerates to walkSpeed in 125/1400 = 89ms and coasts to a stop in
// 125/800 = 156ms, instead of the instant on/off it had, which is what made it read as a
// sprite being dragged rather than a character walking.
constexpr float kPhysMass  = 100.0f;
constexpr float kWalkAccel = 140000.0f / kPhysMass;   // 1400 px/s^2
constexpr float kWalkDrag  =  80000.0f / kPhysMass;   //  800 px/s^2
// Below this the slide is over; without it the avatar creeps for ever on rounding error.
constexpr float kStopEps   = 4.0f;

using t_play_ui_sound   = void(__cdecl*)(const wchar_t*);
using t_ZRefCAvatarAlloc= void*(__thiscall*)(void*);
using t_ZRef_Release    = long(__thiscall*)(void*, int);
using t_AvatarLook_ctor = void*(__thiscall*)(void*, void*);
using t_CAvatar_Init    = void(__thiscall*)(void*, void*, int, void*, void*, int, int, int, int, int);

auto play_ui_sound      = reinterpret_cast<t_play_ui_sound>(kAddr_play_ui_sound);
auto ZRefCAvatar_Alloc  = reinterpret_cast<t_ZRefCAvatarAlloc>(kAddr_ZRefCAvatar_Alloc);
auto ZRef_Release       = reinterpret_cast<t_ZRef_Release>(kAddr_ZRef_Release);
auto AvatarLook_ctor    = reinterpret_cast<t_AvatarLook_ctor>(kAddr_AvatarLook_ctor);
auto CAvatar_Init       = reinterpret_cast<t_CAvatar_Init>(kAddr_CAvatar_Init);

// The AvatarLook slot each equip category occupies, so an UNOWNED cash item can be
// shown on the preview avatar. Lifted verbatim from a sibling window, which
// documents the weapon as the odd one out: a Cash weapon lives in nWeaponStickerID
// rather than in anHairEquip (verified in the look builder at 0x004E7358).
// ONLY 17xxxxx is a weapon sticker. 13xxxxx (one-handed) and 14xxxxx (two-handed) are
// real weapons and belong in anHairEquip[11]; routing them to nWeaponStickerID -- which
// this file did until  -- silently dropped them from the preview, because the
// composer's slot loop (0x004127DF) reads the equip array and the sticker is only
// consulted for the cash overlay. 16xxxxx does not exist in this WZ.
constexpr int kSlotWeaponSticker = -1;
constexpr int kSlotWeapon        = 11;
constexpr int kMaxAvatarSlot     = 11;   // slots 0..11 are what this window can preview

int AvatarSlotOf(int itemId) {
    const int hi = itemId / 100000;
    if (hi == 17) return kSlotWeaponSticker;
    if (hi == 13 || hi == 14) return kSlotWeapon;
    switch (itemId / 10000) {
        case 100: return 1;    // hat
        case 101: return 2;    // face acc
        case 102: return 3;    // eye acc
        case 103: return 4;    // earring
        case 104: case 105: return 5;    // coat / overall (they share the slot)
        case 106: return 6;    // pants
        case 107: return 7;    // shoes
        case 108: return 8;    // glove
        case 109: return 10;   // shield
        case 110: return 9;    // cape
        default:  return 0;    // not previewable on the avatar
    }
}

// The preview look, resolved from the cart BEFORE entering SehBuildAvatar's __try (which
// may not share a frame with anything that unwinds).
//
// Several items at once is safe: the composer's slot loop at 0x004127DF walks all 52
// anHairEquip entries in ascending order and composes every non-zero one, with no
// exclusivity check and no dedup anywhere in the client. Ordering between parts is
// resolved by ART, not logic -- each piece is keyed by its WZ `z` name and drawn in
// Base.wz/zmap.img order.
//
// slot[0] is NOT used for equips: anHairEquip[0] IS the hair, and AvatarSlotOf returns 0
// for "not previewable", so the two would collide. Hair has its own field here.
struct LookOverride {
    int hair    = 0;
    int face    = 0;
    int sticker = 0;
    int slot[kMaxAvatarSlot + 1] = { 0 };   // 1..11; >0 = wear it, <0 = clear it
};

void ApplyItemToLook(LookOverride& lk, int itemId) {
    if (itemId <= 0) return;
    // Hair and face are NOT equips -- they are their own AvatarLook fields. The id
    // classes are the ones facehairunlock.cpp widened them to: Hair {3,4,6}, Face {2,5}.
    const int hi = itemId / 10000;
    if (hi == 3 || hi == 4 || hi == 6) { lk.hair = itemId; return; }
    if (hi == 2 || hi == 5)            { lk.face = itemId; return; }

    const int s = AvatarSlotOf(itemId);
    if (s == kSlotWeaponSticker) { lk.sticker = itemId; return; }
    if (s <= 0 || s > kMaxAvatarSlot) return;

    lk.slot[s] = itemId;                    // last write wins, which is what a cart wants
}

// The overall-versus-coat contest for slot 5 is only settled once EVERY item has been
// applied, so the pants clear has to be decided from the WINNER rather than from whichever
// item happened to be visited. Doing it inside ApplyItemToLook made the clear sticky: a
// coat selected on top of a carted overall stripped the character's real pants, and
// [overall, pants] previewed differently from [pants, overall].
void SettleOverall(LookOverride& lk) {
    // An overall takes the coat slot and covers the legs. Both WOULD compose -- zmap puts
    // mailChest (index 67) in front of pants (72) -- but a SHORT overall lets the pants
    // legs poke through, so clear them the way the server does at equip time.
    if (lk.slot[5] > 0 && lk.slot[5] / 10000 == 105) lk.slot[6] = -1;
}

// =====================================================
// TAXONOMY — Etc.wz/Category.img, verbatim
// =====================================================
// 30 (Category, CategorySub, Name) rows. Tabs 4 and 9 have art in
// UI/CashShop.img/CSTab/Tab but no categories, so they are not shown.
struct CatDef { int tab; int sub; const char* name; };
const CatDef kCats[] = {
    // Tab 1 ("Special": New / Event) is gone. It was never a home category -- v83 used it
    // as a promotional CROSS-LISTING, so everything in it also sits in its real category,
    // which made it a second copy of the shop rather than a section of it.
    { 2, 0,  "Hat"        }, { 2, 1,  "Face"       }, { 2, 2,  "Eye"     },
    { 2, 3,  "Overall"    }, { 2, 4,  "Top"        }, { 2, 5,  "Bottom"  },
    { 2, 6,  "Shoes"      }, { 2, 7,  "Glove"      }, { 2, 8,  "Weapon"  },
    { 2, 9,  "Ring"       }, { 2, 11, "Cape"       },
    // (2,10) "Premium" removed: zero rows in v83 AND zero in the modern catalogue -- it has
    // never had merchandise in either source.
    { 3, 1,  "Messenger"  }, { 3, 2,  "Weather"    },   // (3,0) "Scroll" removed
    { 5, 0,  "Beauty Parlor" }, { 5, 1, "Store"    }, { 5, 2,  "Game"    },
    { 5, 3,  "Facial Expression" }, { 5, 4, "Wedding" }, { 5, 5, "Effect" },
    { 5, 6,  "Character"  },
    { 6, 0,  "Pet"        }, { 6, 1,  "Pet Equip." }, { 6, 2,  "Pet Use" },
    { 7, 0,  "Package"    },
    // Tab 8 ("Guide": How to Use / How to Gift) is deliberately absent. It sold nothing --
    // in v83 its only members are four 403xxxx manual items -- and a tab of instructions
    // has no place in a window that is already the instructions.
};
constexpr int kCatCount = static_cast<int>(_countof(kCats));

// Tab ids in display order, with the labels the stock shop uses for each group.
struct TabDef { int id; const char* name; };
const TabDef kTabs[] = {
    { 2, "Equip" }, { 3, "Use" }, { 5, "Setup" }, { 6, "Pet" }, { 7, "Package" },
};
constexpr int kTabCount = static_cast<int>(_countof(kTabs));

// =====================================================
// LAYOUT
// =====================================================
// 760x540 is the largest that still fits the SMALLEST supported screen mode with a margin:
// resolution.cpp:38 lists 800x600 as index 0 and the fresh-install default (resolution.cpp:181).
// The height is DERIVED from the buy bar rather than chosen: the bar is the last thing in
// the window and it ends flush against the bottom frame, which is what stock does -- the
// Skill window's SKILL POINT band closes on y = H-4, the frame line itself, with no strip
// under it. This window used to reserve a row below the bar for the status line, which read
// as a blank white foot hanging off the bottom. The status moved into the bar; see DrawBuyBar.
// 495, THE PAINTED PLATE'S HEIGHT. The window is no longer drawn from primitives: it blits
// UI/CashShop.img/Base/WndBg, a hand-painted 760x495 plate that carries the frame, the tab
// bands, the red rule, the section-head strips, the well outlines and the footer band. The
// DLL now draws only what CHANGES -- text, icons, tab selection, the avatar and the buttons.
//
// Every geometry constant below was re-derived by measuring that plate rather than adjusted by
// eye. Where the two already agreed the constant is untouched, and most of them did: the grid
// wells land on x = 37 + 86c and y = 106 + 88r in both, and the column split is x = 559 in both.
constexpr int kWndW = 760;
constexpr int kWndH = 495;         // kBarY + kBarH + 4, asserted below once both are known

// LOOK. The chrome is the stock v83 window's, drawn rather than blitted, exactly the way
// a sibling window does it and for the same reason: no vanilla art plate is this size.
// UI/CashShop.img/Base/backgrnd is an 800x600 STAGE backdrop, and the stock window plates
// (UI/UIWindow.img/Skill/backgrnd and friends) are fixed-size canvases with their corners
// baked in, so neither can be stretched to 760x540 without visible damage. What IS
// reusable is the recipe: the frame stack, the caption bevel, the band, the red rule and
// the sunken well are all a handful of 1px rectangles over a white plate, and reproducing
// them from primitives is pixel-identical to the real thing at any size.
//
// Deliberately NOT used: UI/Basic.img/Tab2 for the tab strip, which this window drew
// before. Its SELECTED halves (left1 / right1) are pink in this WZ, so a selected tab came
// out magenta; a sibling window records the same finding. Tabs are drawn instead.
constexpr int kInset  = 3;
constexpr int kFrameX = kInset;
constexpr int kFrameW = kWndW - kInset * 2;

// THE ROUNDED CORNER, measured. Skill/backgrnd, Item/backgrnd and Stat/backgrnd agree to the
// pixel: on the outermost row the first THREE columns are fully transparent, on the next two
// rows one column is, and from the fourth row the edge is square. Every stock window is cut
// this way, and a hard 90-degree corner is one of the plainer tells that a window is custom.
//
// It has to be built by NOT PAINTING those pixels rather than by painting them away:
// IWzCanvas::DrawRectangle blends, so drawing alpha 0 over an opaque pixel is a no-op.
constexpr int kCornerCut[3] = { 3, 1, 1 };

// Title caption: a white box inside a two-tone pale blue bevel, which is exactly the
// stock ITEM INVENTORY / SKILL caption.
// 20, not 21: the decoded caption block is 1px #99BBCC, 1px #8899BB, FOURTEEN rows of flat
// white, 1px #8899BB and THREE rows of #99BBCC. Deliberately asymmetric, and flat -- the
// faint white-to-blue gradient this used to carry is the single loudest modern tell in the
// window, because no stock caption has one.
constexpr int kTitleY    = kInset;
constexpr int kTitleH    = 20;
constexpr int kCloseSize = 12;
constexpr int kCloseX    = kWndW - kInset - 7 - kCloseSize;
// -1: the plate's caption bar is not centred on kTitleH exactly, so the arithmetic centre
// sits a pixel low against the painted bevel.
constexpr int kCloseY    = kTitleY + (kTitleH - kCloseSize) / 2 - 1;

// The tab band and the 3px red rule that closes it. The rule is load-bearing: it is the
// single most recognisable piece of v83 window furniture, and its absence is the first
// thing that makes a custom window read as foreign.
// The proportions are DECODED rather than chosen: a 4px #DDDDDD lip over a 14px body of
// flat #EEEEEE, closed by 1px #AA0000 and 2px #CC0000.
constexpr int kBandY    = kTitleY + kTitleH + 1;
constexpr int kBandLipH = 4;
constexpr int kBandH    = 18;
constexpr int kRedY     = kBandY + kBandH;
constexpr int kRedH     = 3;

constexpr int kTabY     = kBandY + 1;          // inset from the top so it reads as sunk
constexpr int kTabH     = kBandH - 1;
constexpr int kTabX0    = 8;
constexpr int kTabW     = 132;
constexpr int kTabPitch = 134;

// TIER 2: THE CATEGORY STRIP.
//
// Categories used to be a 122px sidebar down the left edge. Folding them into a second tab
// row hands that whole width to the grid, which is what takes an item cell from 79px -- where
// 89% of real catalogue names were cut mid-word -- to 86px, where 87% of them fit whole.
//
// THE TIER BREAK. Two tab rows stacked on each other read as one confusing block unless they
// are visibly different depths. The first attempt gave the strip pale chips and failed: the
// chips COVER the band, so the contrast that actually reaches the eye is chip-against-chip,
// and it measured a channel distance of 60. Stock solves this by not drawing unselected
// chips at all -- a tab strip is a solid band on which only the SELECTION gets a plate --
// which moves the contrast to band-against-band and measures 180.
//
// The band is #99BBCC, the same accent the Skill window's SKILL POINT footer uses, so the
// colour is borrowed from stock furniture rather than invented for this window.
constexpr int kSubY = kRedY + kRedH;
constexpr int kSubH = 23;
// No gap and no maximum width: the tabs TILE the band edge to edge, so both the 3px gutter
// and the 110px cap this used to carry are gone. A tab therefore stretches to whatever share
// of the row its tab's category count gives it, up to the whole 754 when a tab has only one.
// See SubTabX for how the row is divided without leaving a sliver at either end.

// THE INVENTORY'S TAB PLATES, measured off Item/New/Tab0/0 and Tab1/0.
//
// Both are 34 wide and must stretch to a full share of the band, so they are 3-sliced: the caps
// carry the rounded corners and the border, and ONE interior column is repeated across the
// middle. The seams are measured, not guessed. Reading the plates' middle row:
//   unselected  x0 #666666 border, x2-4 a #AAAAAA bevel, x5..28 flat #BBBBBB, x29-31 #CCCCCC
//   selected    x0 #000000, x1 #FFFFFF, x2..9 a highlight ramp #FF6688 -> #FF7799,
//               x10..28 flat #EE6688, x29-31 #DD5577, x32 #FFFFFF, x33 #000000
// so the left cap must be 10 to keep the selected plate's highlight ramp intact, and x=20
// is flat body on BOTH plates, which makes it the one safe fill column.
constexpr int kSubArtW    = 34;
constexpr int kSubArtH[2] = { 18, 19 };   // selected is one row taller: the v83 tab pop
constexpr int kSubCapL    = 10;
constexpr int kSubCapR    = 6;
constexpr int kSubFillX   = 20;
constexpr int kSubBot     = kSubY + 21;   // the row both states' bottoms land on

// The two panes share a top and a bottom; the bar under them carries the buy controls.
constexpr int kPaneTop    = kSubY + kSubH + 3;
constexpr int kPaneBottom = 462;
// 25 and 3. The one column header stock actually ships is a FLAT #CCCCCC strip 25 rows tall
// closed by the 3px red rule: no gradient, no top lip and no #557799 closing line. This
// window's heads used to be an #A8C4D8 -> #8FB0C8 banner, which is furniture v83 has nowhere.
constexpr int kHeadH    = 25;
constexpr int kHeadRedH = 3;
// Both panes start their content the same distance below their own head.
constexpr int kContentDY = kHeadH + kHeadRedH + 2;

// The category SIDEBAR is gone; see kSubY above for what replaced it. Its per-category
// counts went with it: a horizontal chip has room for a name or a number, not both, and the
// number the player is actually browsing is already in the grid header.

// Right pane: the preview column. Placed before the grid because the grid takes what is
// left over. The preview is a PLAYGROUND, not a portrait: the avatar walks inside it, so
// the well is sized to give it room to travel rather than just to frame it.
constexpr int kPrevX = 562, kPrevW = 190;
constexpr int kPlayY = kPaneTop + kContentDY;

// THE BACKDROP is the cash shop's own: UI/CashShop.img/Base/Preview/0, the white MapleStory
// wall with a ladder up the right-hand side. Its two siblings /1 (jungle) and /2 (sky) are
// the same size and the same geometry, so switching is a one-line change.
//
// The art is 212x165 and the well is 182 wide, so it is blitted with a SOURCE CROP rather
// than stretched -- 15px off each side keeps it at 1:1 and keeps the ladder inside. Its
// height is taken as-is, which is why the well is 165 and not the 219 it used to be; the
// spare 54px went to the cart.
constexpr int kPrevArtW = 212, kPrevArtH = 165;
constexpr int kPlayW    = kPrevW - 8;                  // 182
constexpr int kPlayH    = kPrevArtH;
constexpr int kPrevArtSrcX = (kPrevArtW - kPlayW) / 2; // centre the crop
constexpr int kPlayBot  = kPlayY + kPlayH;

// The ground the art paints, measured off the canvas: all three backdrops put it on the
// same row, which is why the client can swap them under one avatar position.
constexpr int kArtGroundY = 134;
constexpr int kFloorY     = kPlayY + kArtGroundY;

// The three backdrops, and the LADDER painted into each. Standing inside the band and
// pressing Up starts a climb -- which is the whole point of it: the client draws a climbing
// avatar from BEHIND, so it is the only way to see the back of a cape, a hat or a weapon
// before buying it.
//
// THESE ARE MEASURED. The previous values were read off a column ruler by eye and every one
// of them sat too far RIGHT, so the avatar climbed beside the rails rather than on them:
//     backdrop 0   was 140..180 (mid 160)   actually 128..171 (mid 149)   11px right
//     backdrop 1   was 140..180 (mid 160)   actually 128..168 (mid 148)   12px right
//     backdrop 2   was 148..192 (mid 170)   actually 122..171 (mid 146)   24px right
// On backdrop 2 the old midpoint landed ON the right rail, which is what made it obvious.
//
// A plain dark-column scan finds backdrop 0 and nothing else, because the other two are dark
// art on dark backgrounds. What works on all three is LOCAL CONTRAST: a rail is a column that
// differs from the art 8px to its left AND 8px to its right at the same row, scored down the
// span where the ladder is isolated against sky. The centre is then the score-weighted median
// of those columns and the rails are the outermost within 40px of it, which ignores the
// foliage on backdrop 2 that a plain min/max picks up.
constexpr int kBgCount = 3;
constexpr int kArtLadderX0[kBgCount] = { 128, 128, 122 };
constexpr int kArtLadderX1[kBgCount] = { 171, 168, 171 };
constexpr int kLadderTopY  = kPlayY + 24;              // where the painted rungs run out
constexpr float kClimbPxPerSec = 62.0f;

// The backdrop picker, three small tabs parked in the PREVIEW header's right end.
constexpr int kBgTabW = 15, kBgTabH = 13, kBgTabGap = 2;
constexpr int kBgTabsW = kBgCount * kBgTabW + (kBgCount - 1) * kBgTabGap;
constexpr int kBgTabX0 = kPrevX + kPrevW - 7 - kBgTabsW;
// THE FOCUS LAMP. "ACTIVE" used to be spelled out here, which cost 44px of header to say one
// bit. A lamp says the same thing in 7px and reads without being parsed, so it sits right of
// the plate's baked PREVIEW label rather than competing with the backdrop tabs for the right
// end. Green when the window has the keyboard, red when it does not -- it is drawn in BOTH
// states on purpose: a lamp that vanishes when unfocused is indistinguishable from a lamp
// that is not there, and the whole point is telling the player where their arrow keys go.
constexpr int kLampX = kPrevX + 68;        // just past the baked "PREVIEW", measured off the art
constexpr int kLampSize = 7;
// +2: the plate's PREVIEW header is not centred on kHeadH exactly, so the arithmetic
// centre sits high against the painted strip.
constexpr int kBgTabY  = kPaneTop + (kHeadH - kBgTabH) / 2 + 2;
inline int BgTabX(int i) { return kBgTabX0 + i * (kBgTabW + kBgTabGap); }

// THE CART: a strip of stock inventory wells, 5 across and 2 down. Double-clicking a
// grid item pins it here and onto the preview avatar; double-clicking a well removes it.
// 10 is the cap ON PURPOSE -- it is what fits without a scrollbar, and it is comfortably
// above the 12 avatar slots' worth of cosmetics anyone tries on at once.
//
// This replaces the CONTROLS help panel that used to sit here. The keys are the player's
// own (see ClassifyKey), so a legend for them was restating what the player already knows.
constexpr int kCartMax  = 20;
constexpr int kCartCols = 5;
constexpr int kCartBox  = 34;                      // measured off the plate's wells
constexpr int kCartGap  = 2;                       // 34 + 2 keeps the painted 36px pitch
// The cart's own head is SHORT: it labels a strip inside a pane rather than opening one, so
// it takes a plain 18px #CCCCCC strip and a hairline instead of the full 25 + red rule a
// pane head carries. Using the pane head here would put a second red rule halfway down the
// preview column, which reads as the window starting over.
constexpr int kCartHeadH = 18;
constexpr int kCartHeadY = kPlayBot + 4;
constexpr int kCartY     = kCartHeadY + kCartHeadH + 6;
constexpr int kCartX0    = kPrevX + 7;             // 569, straight off the plate
constexpr int kCartRows  = 4;
constexpr int kCartH     = kCartRows * kCartBox + (kCartRows - 1) * kCartGap + 4;

// Cash is now ONE line at the column's foot. The CASH section head and the 38px two-column
// readout under it were spending 63 vertical pixels on two numbers; the buy band that used
// to sit below them has moved out to the full-width bar, so the whole tail of this column
// collapses to a hairline and a row of text.
// The NX and MP readouts. Their captions and their recessed value fields are painted into the
// plate; these are the fields' right edges and the row the numbers sit on, all measured off it.
constexpr int kCashValueY = 445;
// WndBg's origin is (0,0), so these are plate pixels: the NX well's grey runs x=588..656 and
// the MP well's x=683..751, each with a 1px light rim to the right. Numbers end kCashPadR inside
// the grey so they sit against the right like every stock readout without touching the rim.
constexpr int kCashPadR = 3;
constexpr int kCashNxRight = 656 - kCashPadR;
constexpr int kCashMpRight = 751 - kCashPadR;
// Usable width of each well from the same padding on the left (588+3 .. 653).
// NX from a billion up (the uncap goes to 9,999,999,999,999) is denominated by FormatCashFit.
constexpr int kCashFieldW = kCashNxRight - (588 + kCashPadR);

// THE BUTTONS ARE THE NPC SHOP'S OWN BUY BUTTON, 3-sliced.
//
// UI/UIWindow.img/Shop/BtBuy is the control the vanilla store puts under its item list: 80x18,
// orange, four states. It is the canonical v83 purchase button, so using it means this window's
// buy action looks like every other buy action in the game.
//
// It replaces ColorPrism/BtConfirm and BtReset, which were 60x20 and wrong in a way that is
// only obvious with the real ones laid beside them: EVERY genuine button carries a 1px soft
// grey bleed outside its hard 1px border, and that is what reads as a raised bevel. The
// ColorPrism pair have neither, so they sit flat on the band, and BtReset is magenta -- a
// colour no stock button uses.
//
// BOTH buttons take the same art, which is what the shop itself does: BUY ITEM and SELL ITEM
// are the same orange and differ only by their label, so giving BUY CART a second colour would
// invent a distinction v83 does not draw.
//
// Each bakes its own label into the middle, which is why the middle is thrown away: blit the
// left cap, repeat one plain face column across the middle, blit the right cap, and the result
// is a BLANK button of the same family at ANY width, in all four of the art's states. The label
// then goes on top in white, the colour the art's own baked glyphs use.
constexpr int kBtnArtW = 80, kBtnArtH = 18;
// The caps are MEASURED, not eyeballed: the tiled body column has to equal the column the right
// cap joins to, or the join shows as a light notch near the right edge. Sweeping every cap pair
// from (3,3) to (13,13) against every glyph-free fill column, (3, 3) with the fill taken at x=3
// gives a per-channel seam error of EXACTLY ZERO on this art.
//
// 3 is enough because the button's whole visible edge lives in its first three columns: x0 is
// the soft grey bleed, x1 the hard border and x2 is already face. The baked "BUY ITEM" glyphs
// do not start until x=19, so x=3 is plain face and safe to repeat.
constexpr int kBtnCapL = 3, kBtnCapR = 3;
constexpr int kBtnH   = kBtnArtH;
constexpr int kBtnGap = 6;
// THE BUTTONS MOVED INTO THE PREVIEW COLUMN, where the plate puts them: at the foot of the
// cart, directly under what they act on, rather than in the band below.
//
// 76 is what the labels want: "BUY CART" measures 47px in MapleUI Button04, and a 76px button
// has a 70px face, which is the same proportion stock uses (UtilDlgEx/BtOK is 46px for an 18px
// "OK").
constexpr int kBtnW   = 76;

// THE BUY BAR, full width under both panes.
//
// The buy controls used to live at the foot of the 190px preview column, which capped a
// button at 88px and stacked the selected item's name above them in a space too narrow to
// hold one. Across the full 754px the selection gets its icon, its whole name and its facts
// on one line, and the buttons sit at their natural size with room between them.
//
// Its furniture is the decoded footer band: a #557799 opening rule, a three-row lighter lip
// (#BBCCDD / #CCDDDD / #AABBCC) and then FLAT #99BBCC -- the same band the Skill window puts
// SKILL POINT in, which is where this colour comes from.
// MEASURED OFF THE PLATE: a #557799 rule on row 462, a lighter lip, then flat #99B0C6 through
// row 490, closed by the window's own frame line on 491. The DLL paints none of it now; these
// bounds exist only so the text that sits IN the band can be placed.
constexpr int kBarY = 462;
constexpr int kBarH = 29;              // 462..490
// BACK IN THE FOOTER BAND. The previous plate marked a button strip at the foot of the cart;
// this one puts the NX and MP readouts there instead, so the buttons move down into the band,
// which this plate leaves empty. Still centred under the preview column, so they stay directly
// beneath the cart they act on.
constexpr int kBuyX     = kPrevX + (kPrevW - (2 * kBtnW + kBtnGap)) / 2;
constexpr int kCartBuyX = kBuyX + kBtnW + kBtnGap;
// +1: centred on the band's full height the pair reads a pixel high against the painted lip,
// which is part of the band but not part of its visual interior.
constexpr int kBtnY     = kBarY + (kBarH - kBtnH) / 2 + 3;
// The plate leaves the footer band EMPTY, so the two readouts that lost their place in the
// preview column live there: cash on the left, the status message on the right.
constexpr int kStatusY = kBarY + 5 + (kBarH - 5 - 12) / 2;
// The window ends where the bar ends, plus the 4-row bottom frame. If either the bar's
// position or its height changes, this catches the mismatch at compile time instead of
// leaving a white foot (too tall) or clipping the buttons (too short).
static_assert(kWndH == kBarY + kBarH + 4, "kWndH must close exactly on the buy bar");

constexpr int kAvatarHomeX = kPrevX + kPrevW / 2;
constexpr int kWalkMargin  = 26;               // keeps the sprite inside the well

// Middle pane: the item grid, plus the stock VScr4 scrollbar. Its 15px width and 13px
// arrow caps are the ART'S geometry rather than a layout decision.
// The grid starts at the window's own margin now that nothing is to its left.
constexpr int kGridX = 8;
constexpr int kGridW = kPrevX - 6 - kGridX;    // 548, up from 390
constexpr int kScrollW     = 15;
constexpr int kScrollX     = kGridX + kGridW - kScrollW - 3;
constexpr int kSbArrowH    = 13;
constexpr int kThumbMinH   = 20;
constexpr int kSbThumbSrcH = 25, kSbThumbCap = 6;

// 6 x 4 = 24 a page, one fewer than the old 5 x 5. That single slot buys 7px of cell width,
// and the names are what it pays for: measured against the real catalogue, 79px fitted 11%
// of item names whole and 86px over two lines fits 87%.
constexpr int kCols = 6, kRows = 4, kCellW = 86, kCellH = 88;   // 24 visible slots
constexpr int kCellX0  = kGridX + 4;
constexpr int kCellY0  = kPaneTop + kContentDY;
constexpr int kCellBox = 36;                   // IconInBox centres a 32px icon inside it
constexpr int kCellBoxDX = (kCellW - kCellBox) / 2;
constexpr int kCellBoxDY = 5;
// DrawItemIconForSlot positions icons by their BOTTOM-LEFT baseline, like the game's own
// inventory: to land a 32px icon inside a box whose top-left
// is (bx, by), draw at (bx + 1, by + kIconBaseline).
constexpr int kIconBaseline = 33;

// The SEARCH BOX, parked in the grid header. A category can hold 2200 hats and the grid
// shows 25 at a time, so browsing to a named item is 90 pages of scrolling without it.
// Widened from 150 with the pane: the header is 158px longer than it was, and a search box
// is the one control here that is never too wide.
constexpr int kSearchW = 254, kSearchH = 15;
constexpr int kSearchX = kGridX + kGridW - 6 - kSearchW;
constexpr int kSearchY = kPaneTop + (kHeadH - kSearchH) / 2;
constexpr int kSearchMax = 24;

constexpr int kScrollTop = kCellY0;
constexpr int kScrollBot = kCellY0 + kRows * kCellH;

// PALETTE -- sampled from UI/UIWindow.img/Skill/backgrnd (ARGB4444), the same decode
// a sibling window records, so this window, the Maker window and the stock ones agree
// pixel for pixel rather than each approximating the look separately.
// The stock frame furniture is unchanged -- the #557799 line, the pale blue title bevel and
// the red rule are what make this read as a MapleStory window and they stay exactly as
// sampled. What changed is the INTERIOR, which was white on white on white:
// a #FFFFFF plate, #FFFFFF panes, #FFFFFF cells and #FFFFFF wells, separated only by
// hairlines, with #EEEEEE section heads that were invisible against all of it.
//
// That depth order was itself the problem. Sinking panes into a tinted plate and banding the
// section heads is a MODERN idiom, and stock v83 does none of it. Re-decoding the shipped
// windows settled every question that had been answered by taste:
//
//     caption      14 rows of FLAT WHITE. Not a gradient. No stock caption has one.
//     tab band     4px #DDDDDD lip over 14px FLAT #EEEEEE.
//     section head the one column header stock ships is FLAT #CCCCCC, closed by the red rule.
//     list rows    Skill/skill0 = FLAT #EEEEEE, skill1 selected = FLAT #CCDDEE, separated by
//                  Skill/line = FLAT #99AABB. No edges, no ramps, no borders.
//     footer band  #557799 rule, a 3px lighter lip, then 19 rows of FLAT #99BBCC.
//     panes        stock windows have essentially NO interior boxed borders.
//     plate        WHITE.
//
// So the plate goes back to white, every gradient becomes a flat fill, the boxed #557799
// panes become flat header strips closed by the red rule, and the saturated #8FB6D4
// selection becomes the pale #CCDDEE that Skill/skill1 actually is.
constexpr DWORD kColPlate    = 0xFFFFFFFF;   // window body. Stock plates are white.
constexpr DWORD kColPaneBg   = 0xFFFFFFFF;   // panes are not tinted; they ARE the plate
constexpr DWORD kColShadow   = 0x44000000;   // the soft pixel outside the white highlight
constexpr DWORD kColFrame    = 0xFF557799;   // the frame line every stock window carries
constexpr DWORD kColFiller   = 0xFFCCCCCC;   // the bottom frame's third row, easy to miss
constexpr DWORD kColShade    = 0xFFEEEEEE;   // the tab band body, flat
constexpr DWORD kColRule     = 0xFFDDDDDD;   // the band lip, and every hairline
constexpr DWORD kColLine     = 0xFF99AABB;   // Skill/line, the stock list separator
constexpr DWORD kColAccent   = 0xFF99BBCC;   // pale blue: title bevel, and the category strip
constexpr DWORD kColAccent2  = 0xFF7E9FB8;   // its darker partner
constexpr DWORD kColBevel    = 0xFF8899BB;   // the title box inner bevel, as decoded
constexpr DWORD kColFootLip1 = 0xFFBBCCDD;   // the footer band's three-row lighter lip
constexpr DWORD kColFootLip2 = 0xFFCCDDDD;
constexpr DWORD kColFootLip3 = 0xFFAABBCC;
constexpr DWORD kColRedTop   = 0xFFAA0000;   // top 1px of the 3px rule under the band
constexpr DWORD kColRed      = 0xFFCC0000;   // its remaining 2px
constexpr DWORD kColWhite    = 0xFFFFFFFF;
constexpr DWORD kColSunkTL   = 0xFF8E9AA6;   // recessed-field bevel, top and left
constexpr DWORD kColSelBg    = 0xFFCCDDEE;   // selected row / cell. Skill/skill1, exactly.
constexpr DWORD kColBtnFace  = 0xFFFAFCFD;   // button / unselected-tab gradient top
constexpr DWORD kColBtnFace2 = 0xFFCBD6DF;   // and bottom
constexpr DWORD kColBtnEdgeH = 0xFFFFFFFF;   // button bevel, top and left
constexpr DWORD kColBtnEdgeS = 0xFF9BAEBE;   // and bottom and right
constexpr DWORD kColBtnDis   = 0xFFEDEFF1;   // disabled button face

// The section head, and the look inside a pane. All flat.
constexpr DWORD kColHead     = 0xFFCCCCCC;   // the decoded column header, one flat value
constexpr DWORD kColCellBg   = 0xFFEEEEEE;   // a grid cell. Skill/skill0.
constexpr DWORD kColCellLine = 0xFFDDDDDD;   // and the separator between cells
constexpr DWORD kColRowAlt   = 0xFFE2E9EF;   // list banding
constexpr DWORD kColStageTop = 0xFFDCE8F2;   // the preview well when its art is missing
constexpr DWORD kColStageBot = 0xFFF4F8FB;
constexpr DWORD kColFloor    = 0xFF6E8CA0;   // the ground line, and its shadow
constexpr DWORD kColFloorSh  = 0xFFC7D6E2;
constexpr DWORD kColBand     = 0xFF99BBCC;   // the footer band, flat. 19 rows of it in stock.

constexpr DWORD kColText   = 0xFF27303C;     // body text
constexpr DWORD kColDim    = 0xFF6B7885;     // secondary text
constexpr DWORD kColSelTx  = 0xFF10293A;     // text on a selected row
constexpr DWORD kColHeadTx = 0xFF14313F;     // a header's own label
constexpr DWORD kColHeadT2 = 0xFF35566B;     // and its right-hand counter
constexpr DWORD kColSubTx  = 0xFF1B3746;     // an unselected category, dark on the blue strip
constexpr DWORD kColLampOn  = 0xFF33AA44;    // focus lamp: the window has the keyboard
constexpr DWORD kColLampOff = 0xFFBB4444;    // ... and when it does not
constexpr DWORD kColLampEdge = 0xFF33414D;   // its 1px surround, so it reads on any header
constexpr DWORD kColPrice  = 0xFF1F6F4A;     // money, deliberately OFF the blue axis so it
                                             // stops competing with the chrome
constexpr DWORD kColDisTx  = 0xFFA8B0B8;     // a disabled control's label
constexpr DWORD kColBtnTx  = 0xFFFFFFFF;     // a button label. Sampled from the ColorPrism
                                             // buttons' OWN baked glyphs, which are white:
                                             // #002255 was right for CSList/BtBuy's pale blue
                                             // face and is near-illegible on orange (2.0:1).
constexpr DWORD kColBtnTxD = 0xFF553322;     // the same label when the button is disabled. NOT
                                             // kColBtnDis above, which is the disabled button
                                             // FACE used by the drawn fallback. The
                                             // art's disabled state drops its baked glyph
                                             // entirely, so this is chosen rather than sampled:
                                             // 4.4:1 on the orange face and 2.6:1 on the pink,
                                             // readable but plainly not the white of an active
                                             // button. kColDisTx, the window's usual disabled
                                             // grey, measures 1.2:1 on orange and vanishes.

// One IWzFont per (face, colour): IWzFont carries BOTH, so neither can be varied per call
//. Hence a slot for every combination the window actually draws.
//
// ONE FACE. Dotum 12, for everything: item names, counts, prices, the search box, the category
// tabs -- all of it DATA that has to keep its own case.
//
// There used to be a second, MapleUI Button04 at 15, for the fixed capitalised labels. It has
// no users left. The painted plate letters CASH SHOP / PREVIEW / CART / NX / MP, the button art
// letters BUY / BUY CART (baked by Tools/make_cashshop_buttons.py, which still uses the face --
// at AUTHORING time), the focus lamp replaced ACTIVE, and the sidebar that carried TOTAL is
// gone. So the window no longer needs a TTF present at runtime at all.
constexpr const wchar_t* kFaceBody = L"Dotum";
constexpr int kSizeBody = 12;

enum {
    // ALL DOTUM NOW. Every caps slot this window had is gone: the painted plate letters
    // CASH SHOP / PREVIEW / CART / NX / MP, the button art letters BUY / BUY CART, the focus
    // lamp replaced ACTIVE, and the category sidebar that carried TOTAL was deleted. Nothing
    // is left for a second face to draw, which is why the TTF is no longer loaded at runtime.
    kF_Text = 0, kF_Dim, kF_Sel, kF_Price, kF_Dis, kF_HeadR, kF_SubTx, kF_SubSel,
    kF_Count
};
struct FontSpec { const wchar_t* face; int size; DWORD col; };
constexpr FontSpec kFonts[kF_Count] = {
    { kFaceBody, kSizeBody, kColText   },   // item names, values
    { kFaceBody, kSizeBody, kColDim    },   // hints, counts, the search placeholder
    { kFaceBody, kSizeBody, kColSelTx  },   // the selected item on the blue band
    { kFaceBody, kSizeBody, kColPrice  },   // prices
    { kFaceBody, kSizeBody, kColDisTx  },   // a disabled control's label
    { kFaceBody, kSizeBody, kColHeadT2 },   // a header's right-hand counter
    { kFaceBody, kSizeBody, kColSubTx  },   // an unselected category on the blue strip
    { kFaceBody, kSizeBody, kColWhite  },   // ... and the selected one, on the rose plate
};

// Defined next to OpenWindow; used by the title-bar drag as well.
void ClampToScreen(int& x, int& y);

// =====================================================
// STATE
// =====================================================
struct Entry {
    int itemId = 0;
    int price = 0;
    int count = 1;
    int tab = 0;
    int cat = 0;
    std::string name;
};

// The catalogue, ONE CATEGORY AT A TIME.
//
// The server sends an INDEX on open -- which categories exist and how many items each has --
// and the merchandise for a category only when the player opens it. So the shop's total size
// is bounded by nothing here: what the window holds is whatever the player has browsed.
inline int BucketKey(int tab, int cat) { return (tab << 8) | (cat & 0xFF); }

std::mutex g_mtx;                       // guards everything below, written by the receive thread
std::map<int, std::vector<Entry>> g_buckets;   // bucket key -> its rows
std::map<int, int> g_counts;                   // bucket key -> how many the server says it has
std::set<int> g_loaded;                        // buckets whose rows have arrived
std::set<int> g_pending;                       // buckets requested, reply outstanding
long long g_cash[3] = { 0, 0, 0 };      // nxCredit (long since the NX uncap), maplePoint, nxPrepaid

std::atomic<bool> g_bWantOpen{ false };
std::atomic<bool> g_bCatalogDirty{ false };
std::atomic<bool> g_bCashDirty{ false };      // only the NX/MP readout changed: repaint, keep tooltip/avatar
// Raised by the receive thread when a cart purchase succeeded; the window empties the
// cart on the MAIN thread, because the cart is window state and nothing else may touch it.
std::atomic<bool> g_bCartBought{ false };

// Remembered across opens, like the bag window's position.
bool s_bSavedPos = false;
int  s_savedX = 0, s_savedY = 0;
int  s_nPrevBg = 0;      // the chosen backdrop, remembered across opens

char g_szStatus[96] = "";

// =====================================================
// PACKETS
// =====================================================
void Send(const COutPacket& p) {
    SendClientPacket(p);
}

void SendRequestCatalog() {
    COutPacket p(kCashShopActionOpcode);
    p.Encode1(kAction_RequestCatalog);
    Send(p);
}

// Ask for one category's merchandise. Called when the player first opens that category, not
// on window open -- which is the whole reason the catalogue has no size limit.
void SendRequestCategory(int tab, int cat) {
    COutPacket p(kCashShopActionOpcode);
    p.Encode1(kAction_RequestCategory);
    p.Encode1(static_cast<unsigned char>(tab));
    p.Encode1(static_cast<unsigned char>(cat));
    Send(p);
}

void SendBuy(int itemId) {
    COutPacket p(kCashShopActionOpcode);
    p.Encode1(kAction_Buy);
    p.Encode4(itemId);
    Send(p);
}

// The cart is ALL-OR-NOTHING on the server, so it goes in one packet rather than as N
// single buys: N buys would leave the player half-charged if the 7th one found the
// inventory full.
void SendBuyCart(const int* ids, int n) {
    if (!ids || n <= 0) return;
    COutPacket p(kCashShopActionOpcode);
    p.Encode1(kAction_BuyCart);
    p.Encode1(static_cast<unsigned char>(n));
    for (int i = 0; i < n; ++i) p.Encode4(ids[i]);
    Send(p);
}

// Raw-offset reader. wvs/packet.h's CanRead bounds against the receive ZArray's
// ALLOCATION, which the client grows by doubling, not against the declared length at
// +0x0C -- so on a truncated packet it happily reads stale bytes. This bounds against
// +0x0C, the way sibling windows do.
struct Reader {
    CInPacket* p;
    bool bad = false;
    explicit Reader(CInPacket* pk) : p(pk) {}
    unsigned char* base() const { return reinterpret_cast<unsigned char*>(p); }
    unsigned char* data() const { return *reinterpret_cast<unsigned char**>(base() + 0x8); }
    unsigned int& offset() const { return *reinterpret_cast<unsigned int*>(base() + 0x14); }
    unsigned short length() const { return *reinterpret_cast<unsigned short*>(base() + 0xC); }
    bool Can(size_t n) const { return !bad && data() && offset() + n <= length(); }
    unsigned char Decode1() {
        if (!Can(1)) { bad = true; return 0; }
        unsigned char v = data()[offset()]; offset() += 1; return v;
    }
    short Decode2() {
        if (!Can(2)) { bad = true; return 0; }
        short v = *reinterpret_cast<short*>(data() + offset()); offset() += 2; return v;
    }
    int Decode4() {
        if (!Can(4)) { bad = true; return 0; }
        int v = *reinterpret_cast<int*>(data() + offset()); offset() += 4; return v;
    }
    long long Decode8() {
        if (!Can(8)) { bad = true; return 0; }
        long long v = *reinterpret_cast<long long*>(data() + offset()); offset() += 8; return v;
    }
    // u16 length + raw bytes, matching ByteBufOutPacket.writeString
    // (a sibling window is the canonical reader).
    std::string DecodeStr() {
        std::string s;
        const unsigned short n = static_cast<unsigned short>(Decode2());
        if (bad) return s;
        if (n > kMaxNameLen || !Can(n)) { bad = true; return s; }
        s.assign(reinterpret_cast<const char*>(data() + offset()), n);
        offset() += n;
        return s;
    }
    void Skip2() { offset() += 2; }
};

// =====================================================
// SEH LEAVES  (__try may not share a function with objects that unwind)
// =====================================================
void* SehCharacterData() {
    void* cd = nullptr;
    __try {
        void* ctx = *reinterpret_cast<void**>(kAddr_CWvsContext);
        if (ctx) cd = *reinterpret_cast<void**>(reinterpret_cast<char*>(ctx) + kOff_CharacterDataInCtx);
    } __except (EXCEPTION_EXECUTE_HANDLER) { cd = nullptr; }
    return cd;
}

void* SehCurrentStage() {
    void* s = nullptr;
    __try { s = *reinterpret_cast<void**>(kAddr_CurrentStage); }
    __except (EXCEPTION_EXECUTE_HANDLER) { s = nullptr; }
    return s;
}

bool SehBuildAvatar(unsigned int* pRef, void* charData, IWzGr2DLayer* pLayer,
                    int x, int y, const LookOverride* lk, void** ppOut,
                    int nPackedMA, int nActionCode) {
    *ppOut = nullptr;
    __try {
        void* pAvatar = ZRefCAvatar_Alloc(pRef);
        if (!pAvatar) return false;
        // sizeof(AvatarLook) is 0x1C5 (wvs/avatar.h static_asserts it). This buffer is
        // deliberately larger, not smaller: AvatarLook::operator= (0x00451541) reads
        // through byte 0x1C4 and no further, and shrinking it to a guessed number is
        // exactly where a real overflow would come from.
        unsigned char buf[0x210];
        memset(buf, 0, sizeof(buf));
        AvatarLook_ctor(buf, charData);
        if (lk) {
            if (lk->hair) *reinterpret_cast<int*>(buf + kAL_Hair) = lk->hair;
            if (lk->face) *reinterpret_cast<int*>(buf + kAL_Face) = lk->face;
            // nWeaponStickerID is a cash OVERLAY on the real weapon: the composer skips
            // slot 11 entirely when it is empty, so on an unarmed character the sticker
            // has nothing to sit on and simply will not draw.
            if (lk->sticker) *reinterpret_cast<int*>(buf + kAL_WeaponSticker) = lk->sticker;
            for (int s = 1; s <= kMaxAvatarSlot; ++s) {
                if (lk->slot[s] > 0) {
                    *reinterpret_cast<int*>(buf + kAL_HairEquip0 + 4 * s) = lk->slot[s];
                } else if (lk->slot[s] < 0) {
                    *reinterpret_cast<int*>(buf + kAL_HairEquip0 + 4 * s) = 0;
                }
            }
        }
        // CAvatar::Init consumes two refs on the layer.
        pLayer->AddRef();
        pLayer->AddRef();
        // Init's 3rd argument is a PACKED MOVE ACTION -- see the block at the top. The
        // literal 1 after the two layers is the Z-ORDER (it reaches put_z, vtbl+0xB4, on
        // CAvatar+0x10C8) and the trailing 0 is the EMOTION id (stored at CAvatar+0x10A8,
        // whose only reader feeds CAvatar::SetEmotion) -- neither is an animate flag.
        CAvatar_Init(pAvatar, buf, nPackedMA, pLayer, pLayer,
                     1, x, y, kAvatarScale, 0);
        // AFTER Init, never before: Init issues its own SetMoveAction, which would
        // rebuild over the override.
        if (nActionCode != kNoActionCode) CAvatar_SetActionCode(pAvatar, nActionCode);
        *ppOut = pAvatar;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ppOut = nullptr;
        return false;
    }
    return true;
}

void SehReleaseAvatar(unsigned int* pRef) {
    __try { if (pRef[1]) ZRef_Release(pRef, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Retarget a LIVE avatar's move action. SetMoveAction early-outs when nMA is unchanged
// (0x004520FF `cmp eax,[esi+0x4e8]` / `je`), so this is cheap to call every frame,
// and it rebuilds the action layer itself -- no PrepareActionLayer call needed.
void SehSetMoveAction(void* pAvatar, int nPackedMA) {
    if (!pAvatar) return;
    __try { CAvatar_SetMoveAct(pAvatar, nPackedMA, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Write the CAvatar+0x4EC override. Unlike SetMoveAction this has NO early-out: it
// unconditionally clears and re-prepares the action layers, so the caller edge-guards it.
void SehSetActionCode(void* pAvatar, int nActionCode) {
    if (!pAvatar) return;
    __try { CAvatar_SetActionCode(pAvatar, nActionCode); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- hiding the animation displayer for the length of one client call ------
// SetEmotion does not stop at the face: it tests TSingleton<CAnimationDisplayer> at
// 0x00451DD4 and, when it exists, sprintfs "Etc/EmotionEffect/<name>" and hands the result to
// CAnimationDisplayer::Effect_General (0x00439750) at THE AVATAR'S OWN coordinates. Those coordinates are window-local here,
// so the effect would land at an arbitrary spot in the real map behind the window, anchored
// to COM objects owned by a window that may be destroyed on the next frame.
//
// Both entry points need this, which is why it is a shared pair rather than open-coded:
// the obvious one is SehSetEmotion, but CAvatar::Update ENDS an expression by calling
// SetEmotion(0, -1) itself (0x004534B3), and emotion 0 resolves Etc/EmotionEffect/blink,
// which does exist in this WZ.
//
// The restore is unconditional, including after an exception: leaving the displayer null
// would silently disable every global animation effect in the client. The fields are volatile because MSVC may otherwise keep a
// value modified inside a __try in a register whose contents are indeterminate afterwards.
struct DisplayerHide {
    void** pp;
    volatile void* pSaved;
    volatile bool  bHidden;
};

void HideDisplayer(DisplayerHide& h) {
    h.pp = reinterpret_cast<void**>(kAddr_AnimationDisplayer);
    h.pSaved = nullptr;
    h.bHidden = false;
    __try {
        h.pSaved = *h.pp;
        *h.pp = nullptr;
        h.bHidden = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void UnhideDisplayer(DisplayerHide& h) {
    if (!h.bHidden) return;
    __try { *h.pp = const_cast<void*>(h.pSaved); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    h.bHidden = false;
}

// One animation step. See the kAddr_CAvatar_Update block: exactly one call per Update(),
// never scaled by dt, because the frame countdown is decremented by a hardcoded 30.
void SehUpdateAvatar(void* pAvatar) {
    if (!pAvatar) return;
    // Displayer hidden for the same reason SehSetEmotion hides it: this call is what expires a
    // held expression, and the expiry runs SetEmotion's add-effect path.
    DisplayerHide h;
    HideDisplayer(h);
    __try { CAvatar_Update(pAvatar); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    UnhideDisplayer(h);
}

// Pull a face, with the animation displayer hidden for the length of the call -- see DisplayerHide.
// Of the 15 paid emotions only `oops` (item 5160001, "Panicky") has an EmotionEffect node
// with canvases in this WZ, so that is the one that would visibly drop an animation into
// the map; the rest resolve to nothing. One stray animation is still one too many.
void SehSetEmotion(void* pAvatar, int nEmotion, int tDurationMs) {
    if (!pAvatar) return;
    DisplayerHide h;
    HideDisplayer(h);
    __try { CAvatar_SetEmotion(pAvatar, nEmotion, tDurationMs); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    UnhideDisplayer(h);
}

// Build the effect layer for one item in one pose, or null if this WZ shape is not one the
// window plays. Returns an OWNED reference.
IWzGr2DLayer* CreateEffectLayer(void* pAvatar, int itemId, const wchar_t* action) {
    if (!pAvatar || itemId <= 0) return nullptr;

    IWzPropertyPtr node;
    try {
        wchar_t path[192];
        // Per-action first: those items carry `string action = 1` and an EMPTY
        // effect/default, so probing default first would find a node with no frames.
        if (action) {
            _snwprintf_s(path, _countof(path), _TRUNCATE,
                         L"Item/Cash/0501.img/%08d/effect/%s", itemId, action);
            node = get_rm()->GetObjectA(path).GetUnknown();
        }
        if (!node) {
            _snwprintf_s(path, _countof(path), _TRUNCATE,
                         L"Item/Cash/0501.img/%08d/effect/default", itemId);
            node = get_rm()->GetObjectA(path).GetUnknown();
        }
        if (!node) {
            _snwprintf_s(path, _countof(path), _TRUNCATE,
                         L"Item/Cash/0528.img/%08d/effect", itemId);
            node = get_rm()->GetObjectA(path).GetUnknown();
        }
    } catch (...) { return nullptr; }
    if (!node) return nullptr;

    // No frame 0 means this is a shape the window does not play: the empty effect/default
    // placeholder a per-action item leaves behind for a pose it has no art for, the
    // five-sibling follow-trail of 5010022/024/041/052, or 5010044's `spectrum`
    // afterimage, which has no canvases at all. Bailing here is what keeps those from
    // producing an empty layer.
    IWzPropertyPtr frame0;
    try { frame0 = node->item[L"0"].GetUnknown(); } catch (...) {}
    if (!frame0) return nullptr;

    // `pos` (0 body, 1 face, 2 center, 3 ground) says which anchor the node's origins are
    // measured from. Only the face case has a distinct vector on a DLL-owned avatar.
    int nPos = 0;
    try { nPos = get_int32(node->item[L"pos"], 0); } catch (...) {}
    const size_t offOrigin = (nPos == 1) ? kOff_AvatarFaceOrigin : kOff_AvatarBodyOrigin;

    void* pOrigin = *reinterpret_cast<void**>(reinterpret_cast<char*>(pAvatar) + offOrigin);
    auto* pOverlay = *reinterpret_cast<IWzGr2DLayer**>(
        reinterpret_cast<char*>(pAvatar) + kOff_AvatarLayerUnderFace);
    if (!pOverlay) return nullptr;                 // avatar not laid out yet

    int bFlip = 0;
    try { bFlip = pOverlay->flip; } catch (...) {}  // itemeff.cpp:62 reads the same field

    // One reference each for the callee to consume.
    IWzProperty* pNode = node.GetInterfacePtr();
    if (!pNode) return nullptr;
    pNode->AddRef();
    if (pOrigin) reinterpret_cast<IUnknown*>(pOrigin)->AddRef();
    pOverlay->AddRef();

    void* pLayer = SehCallCreateAnimLayer(pNode, bFlip, pOrigin, pOverlay);
    if (!pLayer) return nullptr;

    auto* pRet = reinterpret_cast<IWzGr2DLayer*>(pLayer);
    // The factory builds the layer but does not start it.
    try { pRet->Animate(GA_REPEAT); } catch (...) {}
    return pRet;
}

// Read the live +0x4EC override back rather than trusting a remembered copy: CAvatar::Update
// clears it itself when a one-shot action runs off its last frame.
int SehReadActionCode(void* pAvatar) {
    int v = kNoActionCode;
    if (!pAvatar) return v;
    __try {
        v = *reinterpret_cast<int*>(reinterpret_cast<char*>(pAvatar) + kOff_ActionCodeOverride);
    } __except (EXCEPTION_EXECUTE_HANDLER) { v = kNoActionCode; }
    return v;
}

// =====================================================
// THE PLAYER'S OWN KEY BINDINGS
// =====================================================
// TSingleton<CFuncKeyMappedMan> at 0x00BED5A0, created by its CreateInstance (0x009F9E98),
// called from CWvsApp::SetUp (0x009F5239, which bypass.cpp already runs through) and alive for the whole process.
//
//   pEntry = (char*)pMan + 4 + scan * 5      // FUNCKEY_MAPPED { char nType; int nID; }
//
// The 5-byte stride is not an inference: FUNCKEY_MAPPED::Decode (0x004E4409) is literally
// DecodeBuffer(pDst, 5), and the load at 0x0094F37B is
// `lea eax,[eax+esi*4]` / `lea esi,[esi+eax+4]` = pMan + scan*5 + 4.
//
// THE INDEX IS A PS/2 SET-1 SCAN CODE, 0..88 -- NOT a virtual key. WM_KEYDOWN's lParam
// carries it in bits 16..23, which is why nothing else about this window's plumbing
// changes. Right Shift (0x36) is aliased onto Left Shift (0x2A) at every stock read site.
//
// nID is UNALIGNED (offset 4 + 5n), so it must be memcpy'd rather than dereferenced.
constexpr uintptr_t kAddr_FuncKeyMappedMan = 0x00BED5A0;
constexpr int kFuncKeyCount        = 89;      // 0x59, the stock loop bound at 0x0074ABE7
constexpr int kFKType_BasicAction  = 5;
constexpr int kFKAction_Attack     = 52;
constexpr int kFKAction_Jump       = 53;

bool FuncKeyIsAction(int scan, int wantId) {
    if (scan == 0x36) scan = 0x2A;                       // right shift aliases left
    if (scan < 0 || scan >= kFuncKeyCount) return false;
    __try {
        void* pMan = *reinterpret_cast<void**>(kAddr_FuncKeyMappedMan);
        if (!pMan) return false;
        const char* e = reinterpret_cast<const char*>(pMan) + 4 + scan * 5;
        if (*e != static_cast<char>(kFKType_BasicAction)) return false;
        int id = 0;
        memcpy(&id, e + 1, sizeof(id));
        return id == wantId;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Reposition a LIVE avatar. RelMove on the position vector is absolute within the
// origin layer's space, the same meaning CWnd::MoveWnd relies on.
void MoveAvatar(void* pAvatar, int x, int y) {
    if (!pAvatar) return;
    try {
        IWzVector2D* pv = *reinterpret_cast<IWzVector2D**>(
            reinterpret_cast<char*>(pAvatar) + kOff_AvatarPosVector);
        if (pv) pv->RelMove(x, y, Ztl_variant_t(), Ztl_variant_t());
    } catch (...) {}
}

// CWndMan::SetFocus(pWndMan, pWndMan+4) -- hands keyboard focus back to the window
// manager, which forwards it to the current stage. MANDATORY on close: CWnd::Destroy
// reaches CWndMan::Unlink (0x009E3454), which sets m_pFocus to NULL rather than
// back to CWndMan, and while it is NULL CWndMan::IsFocused() stays false and the real
// character remains frozen until the next key or click happens to re-focus it.
void RestoreFocusToWndMan() {
    try {
        void* pWndMan = *reinterpret_cast<void**>(0x00BEC20C);  // TSingleton<CWndMan>::ms_pInstance
        if (!pWndMan) return;
        // CWndMan::SetFocus(IUIMsgHandler*) — v95 sym, v83 VA 0x009E3264 (ret 4)
        reinterpret_cast<void(__thiscall*)(void*, void*)>(0x009E3264)(
            pWndMan, reinterpret_cast<char*>(pWndMan) + 4);
    } catch (...) {}
}

bool GetAbsCursor(POINT& sp) {
    sp.x = 0; sp.y = 0;
    void* input = *reinterpret_cast<void**>(kAddr_InputSystem);
    if (!input) return false;
    reinterpret_cast<void(__thiscall*)(void*, POINT*)>(kAddr_GetCursorPos)(input, &sp);
    return true;
}

// =====================================================
// THE WINDOW
// =====================================================
class CUICashShop : public CWnd {
public:
    ZALLOC_GLOBAL                                  // MANDATORY: the engine frees us
    inline static CUICashShop* ms_pInstance = nullptr;
    inline static CRTTI ms_RTTI{ nullptr };

    int m_screenX, m_screenY;
    int m_nTab;                                    // index into kTabs
    int m_nCat;                                    // index into kCats (absolute)

    int m_nSelItemId;                              // and its item, so no lookup is needed
    int m_nScroll;

    int m_bDragging, m_nDragAnchorX, m_nDragAnchorY;
    int m_nClosePressed, m_nCloseHover;
    int m_nBuyPressed, m_nBuyHover;
    int m_nHoverCell;
    int m_bSbDrag, m_nSbGrabDY;                    // scrollbar thumb drag
    void* m_pCurrentStage;

    unsigned int m_avatarRef[2];
    void* m_pAvatar;
    // A HASH of the look currently baked into the preview, not a single id: the cart can
    // hold several items and the dirty test has to notice any of them changing.
    unsigned int m_hAvatarLook;
    bool  m_bAvatarDirty;

    // --- the cart -----------------------------------------------------------
    struct CartEntry { int itemId; int price; };
    CartEntry m_cart[kCartMax];
    int       m_nCartCount;
    // The serials actually handed to the server, snapshotted when BUY CART was pressed.
    // The cart stays live and editable across the round trip, so the reply has to retire
    // WHAT WAS SENT rather than whatever the cart happens to hold when it lands.
    int       m_sentIds[kCartMax];
    int       m_nSentCount;
    int       m_nCartHover;                        // well under the cursor, -1 = none
    int       m_nBuyCartPressed, m_nBuyCartHover;

    // --- the playground -----------------------------------------------------
    int   m_bFocused;                              // the client's own focus, via OnSetFocus
    bool  m_keyL, m_keyR, m_keyU, m_keyD;
    float m_avX, m_avY;                            // avatar feet, window-local
    float m_velX, m_velY;
    bool  m_bAirborne, m_bFacingLeft;
    bool  m_bOnLadder;
    // A REQUEST, not a deadline. The client ends the swing itself (CAvatar::Update writes
    // -1 back to +0x4EC past the last frame), so the window only has to ask for it once
    // and then watch the live override. A wall-clock timer here would fight that: the
    // animation clock is a fixed 30 per Update call, so above 33.3Hz the swing finishes
    // BEFORE any elapsed-time deadline and the pose gets re-pushed -- an endless loop.
    bool  m_bAttackReq;
    DWORD m_tLastStep;
    int   m_nLastMA;                               // last packed move action pushed
    int   m_nLastCode;                             // last +0x4EC override pushed
    // An expression waiting to be struck. It CANNOT be fired where it is requested:
    // selecting an item raises m_bAvatarDirty, and the next Update rebuilds the avatar
    // from scratch at emotion 0, so anything set beforehand is destroyed before it draws.
    // Update applies this after the rebuild instead.
    int   m_nPendingEmotion;

    // --- the search box -----------------------------------------------------
    char  m_szSearch[kSearchMax + 1];
    int   m_nSearchLen;
    bool  m_bSearchActive;

    // The selected item's effect animation. OWNED, and it points at the avatar's own
    // origin vectors, so it MUST be released before the avatar is.
    IWzGr2DLayer* m_pEffectLayer;
    int   m_nEffectItem;                           // what it was built for, 0 = nothing
    int   m_nEffectMA, m_nEffectCode;              // and for which pose

    IWzFontPtr m_pFont[kF_Count];

    IWzCanvasPtr m_pBtClose[4];     // normal, pressed, disabled, mouseOver
    IWzCanvasPtr m_pBtBuy[4];       // Shop/BtBuy - the vanilla store button, BUY
    IWzCanvasPtr m_pBtCart[4];      // ... and the same art again for BUY CART
    IWzCanvasPtr m_pSbPrev, m_pSbNext, m_pSbBase, m_pSbThumb;
    IWzCanvasPtr m_pPrevBg[kBgCount];   // UI/CashShop.img/Base/Preview/0..2
    IWzCanvasPtr m_pWndBg;              // UI/CashShop.img/Base/WndBg, the painted plate
    // The INVENTORY's own tab plates, 3-sliced. Index 0 = unselected, 1 = selected.
    IWzCanvasPtr m_pSubTab[2];
    int m_nPrevBg;                      // which one is showing

    // --- Esc -------------------------------------------------------------------
    bool m_bInStack;                    // registered in CWvsContext::m_apStackForTab

    // --- the item tooltip ------------------------------------------------------
    // A raw CUIToolTip (0x514 bytes, padded), constructed lazily on first hover exactly as
    // damageskinpicker.cpp does. m_ttRef is a raw ZRef<GW_ItemSlotBase> {bases, p} filled by
    // CItemInfo::GetItemSlot; it is only rebuilt when the hovered item changes.
    alignas(8) unsigned char m_ttBuf[0x600];
    bool  m_bTtInit;
    int   m_nTtItemId;
    void* m_ttRef[2];
    // The item id drawn in each visible grid cell, snapshotted by DrawGrid, so hovering
    // needs no catalogue lock. 0 = empty cell.
    int   m_cellIds[kRows * kCols];

    void StackRaise();
    void StackRemove();
    void ShowItemTip(int itemId, int rx, int ry);
    void HideItemTip();
    void ReleaseTipItem();

    explicit CUICashShop(int nLeft, int nTop);
    virtual ~CUICashShop() override {
        // The tooltip first, in the picker's order: clearing releases its layer, and the
        // destructor would otherwise leave the engine holding a pointer into m_ttBuf.
        if (m_bTtInit) {
            try { reinterpret_cast<void(__thiscall*)(void*)>(kAddr_ToolTip_Clear)(m_ttBuf); } catch (...) {}
            try { reinterpret_cast<void(__thiscall*)(void*)>(kAddr_ToolTip_Dtor)(m_ttBuf); } catch (...) {}
            m_bTtInit = false;
        }
        ReleaseTipItem();
        ReleaseEffect();                   // BEFORE the avatar: it holds the avatar's vectors
        SehReleaseAvatar(m_avatarRef);
        if (ms_pInstance == this) ms_pInstance = nullptr;
    }

    virtual void Draw(const RECT* pRect) override;
    virtual void OnMouseButton(unsigned int msg, unsigned int wParam, int rx, int ry) override;
    virtual int  OnMouseMove(int rx, int ry) override;
    virtual int  OnMouseWheel(int, int, int nWheel) override {
        // CWnd nWheel is inverted vs WM_MOUSEWHEEL: >0 is wheel-down on this client
        // (same sign the bag windows use). Increase offset toward later rows.
        m_nScroll += (nWheel > 0) ? 1 : -1;
        ClampScroll();
        HideItemTip();                     // the item under a still cursor just changed
        InvalidateRect(nullptr);
        return 1;
    }
    virtual void OnMouseEnter(int bEnter) override {
        CWnd::OnMouseEnter(bEnter);
        if (!bEnter) {
            m_nCloseHover = 0; m_nBuyHover = 0; m_nBuyCartHover = 0;
            m_nHoverCell = -1; m_nCartHover = -1;
            HideItemTip();
        }
    }
    // The engine's idle clear (CWndMan's update tick calls every handler's ClearToolTip
    // once the cursor has been still for 15s).
    virtual void ClearToolTip() override { HideItemTip(); }
    virtual void OnDestroy() override;
    virtual void Update() override;
    virtual const CRTTI* GetRTTI() const override { return &ms_RTTI; }
    virtual int IsKindOf(const CRTTI* pRTTI) const override { return ms_RTTI.IsKindOf(pRTTI); }
    // Every other custom window in this DLL returns 0 here ("movement-pause fix",
    // a sibling window) so the player keeps walking while it is open. This one does
    // the OPPOSITE on purpose, and that single return value is the whole playground
    // mechanism:
    //   * CWndMan::SetFocus (0x009E3264) records us as m_pFocus, so the field player's
    //     CVecCtrlUser::WorkUpdateActive (0x009CBEFB) fails its focus gate and calls SetKeyDir(0,0)
    //     -- the real character stops on the next frame, cleanly, even mid-stride
    //     (dx/dy are pre-zeroed at 0x009CBF45 before the gate).
    //   * CWndMan::OnKeyMessage then delivers every key to OUR OnKey instead of
    //     CField::OnKey (0x00529968), so the real character cannot jump, attack, use a
    //     potion or a quickslot while we hold focus.
    // This is what the stock cash shop does too: CCashShop::OnSetFocus (0x00468CF3) is
    // literally `push 1; pop eax; ret 4`.
    virtual int OnSetFocus(int bFocus) override {
        m_bFocused = bFocus;
        if (!bFocus) { m_keyL = m_keyR = m_keyU = m_keyD = false; }
        // Clicking the shop makes it the next thing Esc closes. Stock never reorders the
        // stack on focus; this window does, by request.
        if (bFocus && m_bInStack) StackRaise();
        InvalidateRect(nullptr);
        return 1;
    }

    // Key DOWN vs UP is lParam bit 31 (a sibling window uses the same test).
    // Auto-repeat arrives as further DOWNs with bit 31 clear, so edges are taken from
    // the transition, never from the raw event -- otherwise a held key would restart
    // the jump every frame.
    virtual void OnKey(unsigned int wParam, unsigned int lParam) override {
        const bool bUp = (lParam & 0x80000000u) != 0;
        // The search box gets first refusal while it has focus. It only claims characters,
        // Backspace, Enter and Escape -- the arrows fall through, so the preview stays
        // drivable with the box open, which is how the Maker window behaves too.
        if (!bUp && m_bSearchActive && HandleSearchKey(wParam)) return;

        // WM_KEYDOWN's lParam carries the PS/2 scan code in bits 16..23, which is exactly
        // how CFuncKeyMappedMan is indexed -- so honouring the player's own bindings costs
        // one shift and no extra plumbing.
        const int scan = static_cast<int>((lParam >> 16) & 0xFF);

        switch (wParam) {
            // The arrows stay hardcoded because that is what the client itself does: the
            // walk keys are read straight off the arrow VKs in CVecCtrl::Update
            // (0x009CC005) and are not part of the func-key table at all.
            case VK_LEFT:  m_keyL = !bUp; return;
            case VK_RIGHT: m_keyR = !bUp; return;
            case VK_UP:    m_keyU = !bUp; return;
            case VK_DOWN:  m_keyD = !bUp; return;
            case VK_ESCAPE: {
                // Esc goes to the client's own close stack, exactly as it does from the
                // field or a stock window: ProcessBasicUIKey closes the TOP entry (this
                // window through the TryCloseUI hook, or whatever was opened after it) and
                // opens the game menu only when nothing is left. The search box above has
                // already had first refusal, so Esc still clears a typed filter first.
                m_keyL = m_keyR = m_keyU = m_keyD = false;
                void* ctx = *reinterpret_cast<void**>(kAddr_CWvsContext);
                if (ctx) reinterpret_cast<int(__thiscall*)(void*, unsigned int, unsigned int)>(
                             kAddr_ProcessBasicUIKey)(ctx, wParam, lParam);
                return;
            }
            default: break;
        }

        // JUMP and ATTACK come from the player's OWN key config, read out of
        // CFuncKeyMappedMan. Basic-action ids 53 and 52 are what CUserLocal's func-key
        // handler dispatches on (the `sub esi,0x32` chain at 0x0094F453).
        const bool bJump = FuncKeyIsAction(scan, kFKAction_Jump);
        const bool bAtk  = FuncKeyIsAction(scan, kFKAction_Attack);
        if (bJump || bAtk) {
            if (!bUp) { if (bJump) StartJump(); else StartAttack(); }
            return;                                    // consume both edges
        }

        // The stock defaults, kept as a FALLBACK rather than as the rule. Space is the
        // interesting one: it jumps in every other MapleStory context but the v83 keymap
        // ships it bound to NPC Chat (action 54), so a pure keymap read would silently
        // take Space away from a player who never rebound anything.
        if (!bUp) {
            if (wParam == VK_SPACE || wParam == VK_MENU || wParam == VK_LMENU) {
                StartJump();
                return;
            }
            if (wParam == VK_CONTROL || wParam == VK_LCONTROL) {
                StartAttack();
                return;
            }
        } else if (wParam == VK_SPACE || wParam == VK_MENU || wParam == VK_LMENU ||
                   wParam == VK_CONTROL || wParam == VK_LCONTROL) {
            return;
        }

        // Anything we do not claim still has to reach the global UI hotkeys (Esc, Tab,
        // Enter, the F-key window toggles) -- while we hold focus, CField::OnKey never
        // runs, so this forward is the only thing keeping them alive.
        void* ctx = *reinterpret_cast<void**>(kAddr_CWvsContext);
        if (ctx) reinterpret_cast<int(__thiscall*)(void*, unsigned int, unsigned int)>(
                     kAddr_ProcessBasicUIKey)(ctx, wParam, lParam);
    }

    // The ladder moves with the backdrop, so these are read per frame rather than baked.
    int LadderLeft()  const { return kPrevX + 4 + (kArtLadderX0[m_nPrevBg] - kPrevArtSrcX); }
    int LadderRight() const { return kPrevX + 4 + (kArtLadderX1[m_nPrevBg] - kPrevArtSrcX); }
    int LadderX()     const { return (LadderLeft() + LadderRight()) / 2; }

    void SelectBackground(int i) {
        if (i < 0 || i >= kBgCount || i == m_nPrevBg) return;
        m_nPrevBg = i;
        s_nPrevBg = i;
        // The rungs just moved out from under the avatar, so step it off rather than leave
        // it climbing thin air.
        if (m_bOnLadder) {
            m_bOnLadder = false;
            m_bAirborne = true;
            m_velY = 0.0f;
        }
        play_ui_sound(L"BtMouseClick");
        InvalidateRect(nullptr);
    }

    void SearchSetActive(bool on) {
        if (on == m_bSearchActive) return;
        m_bSearchActive = on;
        InvalidateRect(nullptr);
    }
    void SearchChanged() {
        m_nScroll = 0;
        m_nSelItemId = 0;
        m_bAvatarDirty = true;
        ClampScroll();
        InvalidateRect(nullptr);
    }
    void SearchClear() {
        if (!m_nSearchLen) return;
        m_szSearch[0] = 0;
        m_nSearchLen = 0;
        SearchChanged();
    }

    // Returns true when the key was consumed. Translation goes through the ACTIVE keyboard
    // layout rather than assuming US positions, so the box accepts whatever the player's
    // layout actually produces (a sibling window does the same).
    bool HandleSearchKey(unsigned int vk) {
        if (vk == VK_ESCAPE) {
            if (m_nSearchLen) SearchClear(); else SearchSetActive(false);
            return true;
        }
        if (vk == VK_RETURN) { SearchSetActive(false); return true; }
        if (vk == VK_BACK) {
            if (m_nSearchLen > 0) {
                m_szSearch[--m_nSearchLen] = 0;
                SearchChanged();
            }
            return true;
        }
        char ch = 0;
        BYTE ks[256];
        if (GetKeyboardState(ks)) {
            WORD outCh = 0;
            const UINT scan = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
            const int r = ToAscii(vk, scan, ks, &outCh, 0);
            if (r == -1) return true;                 // dead accent buffered by the layout
            if (r >= 1) ch = static_cast<char>(outCh & 0xFF);
        }
        if (!ch || static_cast<unsigned char>(ch) < 0x20) return false;   // let arrows through
        if (m_nSearchLen >= kSearchMax) return true;
        m_szSearch[m_nSearchLen++] = ch;
        m_szSearch[m_nSearchLen] = 0;
        SearchChanged();
        return true;
    }

    void StartJump() {
        // Jumping off the rungs is the quick way down, and the only way off sideways.
        if (m_bOnLadder) {
            m_bOnLadder = false;
            m_bAirborne = true;
            m_velY = kJumpVel0;
            return;
        }
        if (m_bAirborne) return;
        m_bAirborne = true;
        m_velY = kJumpVel0;
    }
    void StartAttack() {
        // No swing on this build, so do not root the avatar for nothing.
        if (g_nSwingCode == kNoActionCode) return;
        m_bAttackReq = true;                // consumed by the next StepPlayground
    }
    void ApplyPose(const Pose& p);
    void StepPlayground(DWORD tNow);

    static IWzCanvasPtr LoadSprite(const wchar_t* p) {
        IWzCanvasPtr c;
        try { c = get_unknown(get_rm()->GetObjectA(const_cast<wchar_t*>(p))); } catch (...) {}
        return c;
    }
    static void BlitA(IWzCanvasPtr dst, IWzCanvasPtr src, int x, int y) {
        if (dst && src)
            try { dst->CopyEx(x, y, src, CANVAS_ALPHATYPE::CA_OVERWRITE, 0, 0, 0, 0, 0, 0); } catch (...) {}
    }
    void LoadSprites();
    void MakeFonts();

    // --- taxonomy helpers ---------------------------------------------------
    int TabId() const { return kTabs[m_nTab].id; }
    static int TabX(int i) { return kTabX0 + i * kTabPitch; }

    // Rows of kCats that belong to the current tab.
    int CatRowCount() const {
        int n = 0;
        for (int i = 0; i < kCatCount; ++i) if (kCats[i].tab == TabId()) ++n;
        return n;
    }
    int CatIndexOfRow(int row) const {
        int n = 0;
        for (int i = 0; i < kCatCount; ++i) {
            if (kCats[i].tab != TabId()) continue;
            if (n == row) return i;
            ++n;
        }
        return -1;
    }

    // The tabs TILE the strip: no gaps, and the row spans the band's full width exactly.
    //
    // The edge is computed from the index rather than the width accumulated from a single
    // per-tab figure, because integer division does not divide evenly. 754 across 11
    // categories is 68.5, so a uniform width of 68 would leave a 6px sliver of bare band at
    // the right end and a uniform 69 would overrun by 5. Deriving each boundary as
    // (kFrameW * r) / n spreads the remainder over the row -- some tabs come out one pixel
    // wider than their neighbours, which is invisible -- and makes the last edge land on
    // kFrameX + kFrameW by construction, whatever n is.
    int SubTabX(int r) const {
        const int n = CatRowCount();
        if (n <= 0) return kFrameX;
        if (r < 0) r = 0;
        if (r > n) r = n;
        return kFrameX + (kFrameW * r) / n;
    }
    // The width of tab r is just the distance to the next boundary, so the widths sum to
    // exactly kFrameW and adjacent tabs always touch.
    int SubTabW(int r) const { return SubTabX(r + 1) - SubTabX(r); }

    // Catalog entries under (tab, category). Caller holds g_mtx.
    //
    // Copies BY VALUE rather than handing back pointers into the bucket: the receive
    // thread appends to that vector, and a reallocation there would dangle every
    // pointer a draw or a click was still holding after it dropped the lock.
    void CollectLocked(std::vector<Entry>& out) const {
        out.clear();
        if (m_nCat < 0 || m_nCat >= kCatCount) return;
        const CatDef& c = kCats[m_nCat];
        auto it = g_buckets.find(BucketKey(c.tab, c.sub));
        if (it == g_buckets.end()) return;
        // The filter is applied HERE rather than at draw time on purpose: the grid, the
        // scrollbar, ClampScroll and every hit test all read this, and filtering in only
        // one of them would leave the click handler indexing a different list from the one
        // on screen.
        if (m_nSearchLen == 0) { out = it->second; return; }
        for (const Entry& e : it->second)
            if (NameContains(e.name, m_szSearch)) out.push_back(e);
    }

    // Case-insensitive substring, ASCII. Item names in this catalogue are ASCII by
    // construction -- the server writes them with a US-ASCII charset.
    static bool NameContains(const std::string& hay, const char* needle) {
        if (!needle || !*needle) return true;
        const size_t n = strlen(needle);
        if (hay.size() < n) return false;
        for (size_t i = 0; i + n <= hay.size(); ++i) {
            size_t j = 0;
            while (j < n && tolower(static_cast<unsigned char>(hay[i + j]))
                          == tolower(static_cast<unsigned char>(needle[j]))) ++j;
            if (j == n) return true;
        }
        return false;
    }

    // What the INDEX says this category holds, which is known before its rows arrive -- so
    // the left column shows real counts immediately instead of zeroes.
    static int CountOf(int tab, int cat) {
        auto it = g_counts.find(BucketKey(tab, cat));
        return (it == g_counts.end()) ? 0 : it->second;
    }
    bool CurrentCategoryLoaded() const {
        if (m_nCat < 0 || m_nCat >= kCatCount) return true;
        const CatDef& c = kCats[m_nCat];
        std::lock_guard<std::mutex> lk(g_mtx);
        return g_loaded.count(BucketKey(c.tab, c.sub)) != 0;
    }
    // Request the current category if it has not been asked for yet. Idempotent: g_pending
    // stops a second request going out while the first is still in flight, which matters
    // because clicking down a category list fires one of these per click.
    void EnsureCategoryRequested() {
        if (m_nCat < 0 || m_nCat >= kCatCount) return;
        const CatDef& c = kCats[m_nCat];
        const int key = BucketKey(c.tab, c.sub);
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (g_loaded.count(key) || g_pending.count(key)) return;
            g_pending.insert(key);
        }
        SendRequestCategory(c.tab, c.sub);
    }
    int VisibleCount() const {
        std::lock_guard<std::mutex> lk(g_mtx);
        std::vector<Entry> v;
        CollectLocked(v);
        return static_cast<int>(v.size());
    }
    void ClampScroll() {
        const int total = VisibleCount();
        int maxRow = (total + kCols - 1) / kCols - kRows;
        if (maxRow < 0) maxRow = 0;
        if (m_nScroll > maxRow) m_nScroll = maxRow;
        if (m_nScroll < 0) m_nScroll = 0;
    }

    void SelectTab(int i);
    void SelectCat(int absIndex);
    void SelectItem(int itemId);
    void RebuildAvatar();

    // --- the cart -----------------------------------------------------------
    int  CartIndexOfItem(int itemId) const {
        for (int i = 0; i < m_nCartCount; ++i)
            if (m_cart[i].itemId == itemId) return i;
        return -1;
    }
    void CartToggle(int itemId, int price);
    void BuildLook(LookOverride& out) const;
    int  CartTotalPrice() const;

    // Remove exactly the serials that were sent. Anything double-clicked in during the
    // round trip was never bought and stays in the cart.
    void CartRetireSent() {
        for (int i = 0; i < m_nSentCount; ++i) {
            const int at = CartIndexOfItem(m_sentIds[i]);
            if (at < 0) continue;
            for (int j = at; j + 1 < m_nCartCount; ++j) m_cart[j] = m_cart[j + 1];
            --m_nCartCount;
        }
        m_nSentCount = 0;
        m_bAvatarDirty = true;
    }

    // --- the effect layer ---------------------------------------------------
    void ReleaseEffect() {
        if (m_pEffectLayer) {
            try { m_pEffectLayer->Release(); } catch (...) {}
            m_pEffectLayer = nullptr;
        }
        m_nEffectItem = 0;
        m_nEffectMA   = -1;
        m_nEffectCode = kNoActionCode;
    }
    void RefreshEffect();

    // --- the drawing vocabulary ---------------------------------------------
    // Every stock-looking piece in this window is built out of these five primitives.
    // They are a sibling window's, verbatim, so the two windows cannot drift apart.
    void Fill(IWzCanvasPtr c, int x, int y, int w, int h, DWORD col) {
        if (!c || w <= 0 || h <= 0) return;
        try { c->DrawRectangle(x, y, w, h, col); } catch (...) {}
    }
    // Vertical gradient, one DrawRectangle per scanline. Alpha is forced opaque.
    void FillV(IWzCanvasPtr c, int x, int y, int w, int h, DWORD top, DWORD bot) {
        if (!c || w <= 0 || h <= 0) return;
        if (h == 1) { Fill(c, x, y, w, 1, top); return; }
        for (int i = 0; i < h; ++i) {
            const int num = i, den = h - 1;
            const DWORD r = (((top >> 16) & 0xFF) * (den - num) + ((bot >> 16) & 0xFF) * num) / den;
            const DWORD g = (((top >>  8) & 0xFF) * (den - num) + ((bot >>  8) & 0xFF) * num) / den;
            const DWORD b = (((top      ) & 0xFF) * (den - num) + ((bot      ) & 0xFF) * num) / den;
            Fill(c, x, y + i, w, 1, 0xFF000000u | (r << 16) | (g << 8) | b);
        }
    }
    void Border(IWzCanvasPtr c, int x, int y, int w, int h, DWORD col) {
        Fill(c, x, y, w, 1, col);
        Fill(c, x, y + h - 1, w, 1, col);
        Fill(c, x, y, 1, h, col);
        Fill(c, x + w - 1, y, 1, h, col);
    }

    // How many columns the rounded corner cuts from each end of row y, for a box whose top
    // and bottom edges are y0 and y1. Rows more than two from an edge are square.
    int CornerCutAt(int y, int y0, int y1) {
        const int d = (y - y0 < y1 - y) ? (y - y0) : (y1 - y);
        return (d >= 0 && d < 3) ? kCornerCut[d] : 0;
    }

    // A 1px outline with the stock rounded corner. The top and bottom runs are inset by the
    // outermost cut; the sides step in along the corner staircase.
    void RoundRing(IWzCanvasPtr c, int x0, int y0, int x1, int y1, DWORD col) {
        const int c0 = kCornerCut[0];
        Fill(c, x0 + c0, y0, (x1 - x0 + 1) - 2 * c0, 1, col);
        Fill(c, x0 + c0, y1, (x1 - x0 + 1) - 2 * c0, 1, col);
        for (int y = y0 + 1; y < y1; ++y) {
            const int k = CornerCutAt(y, y0, y1);
            Fill(c, x0 + k, y, 1, 1, col);
            Fill(c, x1 - k, y, 1, 1, col);
        }
    }

    // The window body, with the same corner left unpainted so the plate never pokes out
    // past the ring drawn over it.
    void FillRounded(IWzCanvasPtr c, int x0, int y0, int x1, int y1, DWORD col) {
        for (int d = 0; d < 3; ++d) {
            const int k = kCornerCut[d];
            const int w = (x1 - x0 + 1) - 2 * k;
            Fill(c, x0 + k, y0 + d, w, 1, col);
            Fill(c, x0 + k, y1 - d, w, 1, col);
        }
        Fill(c, x0, y0 + 3, x1 - x0 + 1, (y1 - y0 + 1) - 6, col);
    }
    // A recessed field: the bevel every stock text box, readout and inventory cell uses.
    void DrawSunken(IWzCanvasPtr c, int x, int y, int w, int h) {
        Fill(c, x, y, w, h, kColWhite);
        Fill(c, x, y, w, 1, kColSunkTL);          // top   and
        Fill(c, x, y, 1, h, kColSunkTL);          // left  are the shadowed edges
        Fill(c, x, y + h - 1, w, 1, kColWhite);
        Fill(c, x + w - 1, y, 1, h, kColWhite);
    }

    void Str(IWzCanvasPtr c, int fi, int x, int y, const char* s) {
        if (fi < 0 || fi >= kF_Count) return;
        IWzFont* f = m_pFont[fi];
        if (!c || !f || !s || !*s) return;
        try { c->DrawTextA(x, y, Ztl_bstr_t(s), f, Ztl_variant_t(), Ztl_variant_t()); } catch (...) {}
    }
    int TextW(int fi, const char* s) {
        if (fi < 0 || fi >= kF_Count || !s) return 0;
        IWzFont* f = m_pFont[fi];
        if (!f) return static_cast<int>(strlen(s)) * 6;
        try { return static_cast<int>(f->CalcTextWidth(Ztl_bstr_t(s), Ztl_variant_t())); }
        catch (...) { return static_cast<int>(strlen(s)) * 6; }
    }
    void StrRight(IWzCanvasPtr c, int fi, int right, int y, const char* s) {
        Str(c, fi, right - TextW(fi, s), y, s);
    }
    // Billions and up are denominated: up to two decimals of the unit, truncated (never rounded
    // up, so it never reads as more than the balance) with trailing zeros dropped --
    // 5,120,000,000 -> "5.12b", 5,000,000,000 -> "5b", 9,999,999,999,999 -> "9.99t".
    // Below a billion the whole number is shown, falling back to m/k only if it cannot fit maxW.
    void FormatCashFit(char* out, size_t n, long long value, int maxW) {
        static const struct { long long div; char unit; } kUnits[] = {
            { 1000000000000LL, 't' }, { 1000000000LL, 'b' }, { 1000000LL, 'm' }, { 1000LL, 'k' } };
        _snprintf(out, n, "%lld", value); out[n - 1] = 0;
        const bool bFits = value < 1000000000LL && TextW(kF_Text, out) <= maxW;
        if (bFits) return;
        for (const auto& u : kUnits) {
            if (value >= u.div) {
                const long long whole = value / u.div;
                const long long frac = (value % u.div) / (u.div / 100);
                if (frac == 0) {
                    _snprintf(out, n, "%lld%c", whole, u.unit);
                } else if (frac % 10 == 0) {
                    _snprintf(out, n, "%lld.%lld%c", whole, frac / 10, u.unit);
                } else {
                    _snprintf(out, n, "%lld.%02lld%c", whole, frac, u.unit);
                }
                out[n - 1] = 0;
                return;
            }
        }
    }
    void StrCenter(IWzCanvasPtr c, int fi, int cx, int y, const char* s) {
        Str(c, fi, cx - TextW(fi, s) / 2, y, s);
    }
    void Clip(int fi, char* s, int maxW) {
        while (*s && TextW(fi, s) > maxW) s[strlen(s) - 1] = 0;
    }

    // Centres a 32px icon in a box of any size. Guarded per call: DrawItemIconForSlot
    // (0x005D6458) reaches GetEquipItem (0x005CA785) and raises _com_error E_FAIL for an
    // id whose icon node is missing. Unguarded that unwinds out of Draw and abandons every
    // later step, so one bad icon would blank the rest of the window
    void IconInBox(IWzCanvasPtr c, int itemId, int boxX, int boxY, int size) {
        auto* pII = CItemInfo::GetInstance();
        if (!pII || !c) return;
        const int off = (size - 32) / 2;
        try {
            pII->DrawItemIconForSlot(c, itemId, boxX + off + 1, boxY + off + kIconBaseline,
                                     0, 0, 0, 0, 0, 0);
        } catch (...) {
            LOG_ONCE("cashshopwnd: icon draw threw for item %d", itemId);
        }
    }

    // --- the composed pieces ------------------------------------------------
    void DrawSectionHead(IWzCanvasPtr c, int x, int y, int w, const char* left, const char* right);
    void DrawTab(IWzCanvasPtr c, int x, int y, int w, int h, bool sel, bool topOnly);
    void DrawButton(IWzCanvasPtr c, int x, int y, int w, int h, const char* label,
                    bool enabled, bool pressed, bool hover);
    void DrawVanillaButton(IWzCanvasPtr c, int x, int y, int w, const char* label,
                           bool enabled, bool pressed, bool hover, IWzCanvasPtr* art);
    void DrawFrame(IWzCanvasPtr c);
    void DrawTabs(IWzCanvasPtr c);
    void DrawCategories(IWzCanvasPtr c);
    void DrawSubTab(IWzCanvasPtr c, int x, int y, int w, bool sel);
    void DrawGrid(IWzCanvasPtr c, const std::vector<Entry>& vis);
    void DrawScrollbar(IWzCanvasPtr c, int total);
    void DrawPreview(IWzCanvasPtr c, const std::vector<Entry>& vis);
    void DrawBuyBar(IWzCanvasPtr c);
    void ThumbRect(RECT& out, int total) const;
    void DragThumbTo(int localY);
};

// ---------------------------------------------------------------------------
// A FLAT #CCCCCC strip closed by the 3px red rule -- the exact anatomy of the only column
// header v83 ships (the Shop list's). It used to be an #A8C4D8 -> #8FB0C8 gradient banner
// closed by a #557799 hairline, which is a shape stock has nowhere.
//
// The red rule belongs to the head, not to the window: repeating the band's own rule at the
// top of each pane is what ties the panes to the chrome above them.
void CUICashShop::DrawSectionHead(IWzCanvasPtr c, int x, int y, int w,
                                  const char* left, const char* right) {
    // The strip and its closing red rule are painted into the plate; only the labels are drawn.
    // The LEFT label is not drawn. "PREVIEW" is baked into the plate, and the grid's was the
    // current category's name -- which the selected tab in the strip directly above already
    // says, so printing it again was saying the same word twice in two rows.
    (void)left;
    if (right && *right) StrRight(c, kF_HeadR, x + w - 7, y + (kHeadH - 12) / 2, right);
}

// Raised and white when selected, sunk into the band when not. topOnly leaves the bottom
// edge open so the selected tab reads as joined to the body below it. a sibling window.
void CUICashShop::DrawTab(IWzCanvasPtr c, int x, int y, int w, int h, bool sel, bool topOnly) {
    if (sel) {
        Fill(c, x, y, w, h, kColWhite);
        Fill(c, x + 1, y, w - 2, 1, kColFrame);
        Fill(c, x, y + 1, 1, h - 1, kColFrame);
        Fill(c, x + w - 1, y + 1, 1, h - 1, kColFrame);
        if (!topOnly) Fill(c, x, y + h - 1, w, 1, kColFrame);
        Fill(c, x + 1, y + 1, w - 2, 1, kColWhite);
        return;
    }
    // Sunk: FLAT #DDDDDD with a 1px white top row, inset 3px so it reads as pushed down into
    // the band. The gradient face this carried is the modern tell; stock tabs have none.
    Fill(c, x + 1, y + 3, w - 2, h - 3, kColRule);
    Fill(c, x + 1, y + 3, w - 2, 1, kColWhite);
    Fill(c, x, y + 4, 1, h - 4, kColLine);
    Fill(c, x + w - 1, y + 4, 1, h - 4, kColLine);
    if (!topOnly) Fill(c, x, y + h - 1, w, 1, kColLine);
}

// A stock button is a light vertical face inside a #557799 frame, with a 1px white bevel
// top and left and a cool grey one bottom and right. Pressed swaps the bevel and nudges
// the label a pixel, which is the whole of the animation v83 buttons have.
void CUICashShop::DrawButton(IWzCanvasPtr c, int x, int y, int w, int h, const char* label,
                             bool enabled, bool pressed, bool hover) {
    if (!enabled) {
        Fill(c, x, y, w, h, kColBtnDis);
        Border(c, x, y, w, h, kColRule);
        StrCenter(c, kF_Dis, x + w / 2, y + (h - 12) / 2, label);
        return;
    }
    // DARK TOP -> LIGHT BOTTOM. Stock v83 button faces are lit from BELOW: every one sampled
    // runs a 1px light lip and then a monotone dark-to-light ramp (Trunk/BtGet #DD7711 ->
    // #FF9955, Stat/BtDetail #99BB11 -> #CCDD44). Light-top-to-dark-bottom is the modern
    // convention and it was the wrong way up here. Pressed inverts the ramp, as stock does.
    const DWORD top = pressed ? kColBtnFace  : kColBtnFace2;
    const DWORD bot = pressed ? kColBtnFace2 : (hover ? kColWhite : kColBtnFace);
    FillV(c, x + 1, y + 1, w - 2, h - 2, top, bot);
    Border(c, x, y, w, h, kColFrame);
    const DWORD hi = pressed ? kColBtnEdgeS : kColBtnEdgeH;
    const DWORD lo = pressed ? kColBtnEdgeH : kColBtnEdgeS;
    Fill(c, x + 1, y + 1, w - 2, 1, hi);
    Fill(c, x + 1, y + 1, 1, h - 2, hi);
    Fill(c, x + 1, y + h - 2, w - 2, 1, lo);
    Fill(c, x + w - 2, y + 1, 1, h - 2, lo);
    StrCenter(c, kF_Text, x + w / 2, y + (h - 12) / 2 + (pressed ? 1 : 0), label);
}

// The cash shop's own button at an arbitrary width: left cap, the plain face column
// repeated across the middle (which is what erases the baked "BUY"), then the right cap.
// Falls back to the drawn button if the art is missing, so a stripped WZ still gets a button
// rather than a gap.
void CUICashShop::DrawVanillaButton(IWzCanvasPtr c, int x, int y, int w, const char* label,
                                    bool enabled, bool pressed, bool hover, IWzCanvasPtr* art) {
    const int st = !enabled ? 2 : (pressed ? 1 : (hover ? 3 : 0));
    IWzCanvasPtr src = art[st] ? art[st] : art[0];
    if (!src) {
        // No art at all: fall back to the drawn button, which still needs a drawn label.
        DrawButton(c, x, y, w, kBtnArtH, label, enabled, pressed, hover);
        return;
    }
    // The art is already kBtnW wide with its label baked in, so this is a straight blit --
    // no 3-slice, and nothing lettered on top. `label` is kept in the signature for the
    // fallback above and for callers to stay self-documenting at the call site.
    BlitA(c, src, x, y);
    (void)w;
}

void CUICashShop::LoadSprites() {
    // The stock close button, the one every v83 window carries. NOT the Bag window's copy
    // this file used before: Basic.img is where the shared furniture lives, and the four
    // states are what the hover and press feedback below expects.
    m_pBtClose[0] = LoadSprite(L"UI/Basic.img/BtClose/normal/0");
    m_pBtClose[1] = LoadSprite(L"UI/Basic.img/BtClose/pressed/0");
    m_pBtClose[2] = LoadSprite(L"UI/Basic.img/BtClose/disabled/0");
    m_pBtClose[3] = LoadSprite(L"UI/Basic.img/BtClose/mouseOver/0");

    // The stock vertical scrollbar. Its geometry is the art's, not a choice: the arrows
    // and the thumb caps are fixed-size sprites and only the middles stretch.
    m_pSbPrev  = LoadSprite(L"UI/Basic.img/VScr4/enabled/prev0");
    m_pSbNext  = LoadSprite(L"UI/Basic.img/VScr4/enabled/next0");
    m_pSbBase  = LoadSprite(L"UI/Basic.img/VScr4/enabled/base");
    m_pSbThumb = LoadSprite(L"UI/Basic.img/VScr4/enabled/thumb0");

    // The cash shop's own preview backdrops: a white MapleStory wall, a jungle and a sky.
    // All three are 212x165 with their ground on the same row, so only the ladder column
    // differs between them -- which is what kArtLadderX0/X1 carry.
    // THE PAINTED PLATE. Carries the frame, the caption, both tab bands, the red rule,
    // the section-head strips, every well outline and the footer band. If it is missing the
    // window still works: DrawFrame falls back to drawing the chrome from primitives.
    // Custom.wz: the plate, BtBuy and BtCart are NOT in the client's UI.wz. resman's merge would
    // surface the WndBg canvas at the stock path, but never the BtBuy/BtCart sub-properties, so
    // all three are read through the explicit Custom/ namespace.
    m_pWndBg = LoadSprite(L"Custom/UI/CashShop.img/Base/WndBg");

    m_pPrevBg[0] = LoadSprite(L"UI/CashShop.img/Base/Preview/0");
    m_pPrevBg[1] = LoadSprite(L"UI/CashShop.img/Base/Preview/1");
    m_pPrevBg[2] = LoadSprite(L"UI/CashShop.img/Base/Preview/2");

    // THE ITEM INVENTORY'S OWN TAB PLATES, for the category strip. These are the plates
    // behind its Equip / Use / Set-up / Etc / Cash tabs: Item/New/Tab0 is the unselected
    // set and Tab1 the selected one, five of each (one per tab slot, differing only in
    // shade). Index 0 of each is taken, which is the Equip slot.
    //
    // NOT UI/Basic.img/Tab, which is the shared strip a first pass used: that is a different
    // control (pale blue and white, with an angled right cap) and it is not what the
    // inventory draws. These are flat rounded plates, grey when unselected and ROSE when
    // selected -- the same rose the rest of this UI now carries.
    //
    // They carry no baked background, only anti-aliased corners, so they blit whole. The
    // selected plate is 34x19 against the unselected 34x18: one row taller, which is the
    // v83 tab pop and the reason the two are drawn to a common bottom.
    m_pSubTab[0] = LoadSprite(L"UI/UIWindow.img/Item/New/Tab0/0");
    m_pSubTab[1] = LoadSprite(L"UI/UIWindow.img/Item/New/Tab1/0");

    // The cash shop's own button, 3-sliced to any width by DrawVanillaButton.
    // THE WINDOW'S OWN BUTTONS, lettered in the art rather than at runtime.
    //
    // Tools/make_cashshop_buttons.py bakes these from Shop/BtBuy: sliced to kBtnW, its own
    // "BUY ITEM" erased by the fill repeat, then "BUY" / "BUY CART" drawn on top in MapleUI
    // Button04. That face was the LAST thing this window needed a TTF for -- the painted plate
    // absorbed every other caps label -- so baking the two strings removes the font from the
    // runtime entirely. It is also steadier: the label lands on identical pixels in all four
    // states instead of being re-centred by whatever face the client manages to resolve.
    static const wchar_t* kBtnStates[4] = { L"normal", L"pressed", L"disabled", L"mouseOver" };
    for (int i = 0; i < 4; ++i) {
        wchar_t path[96];
        _snwprintf(path, 96, L"Custom/UI/CashShop.img/Base/BtBuy/%s", kBtnStates[i]);
        path[95] = 0;
        m_pBtBuy[i] = LoadSprite(path);
        _snwprintf(path, 96, L"Custom/UI/CashShop.img/Base/BtCart/%s", kBtnStates[i]);
        path[95] = 0;
        m_pBtCart[i] = LoadSprite(path);
    }
}

void CUICashShop::MakeFonts() {
    for (int i = 0; i < kF_Count; ++i) {
        m_pFont[i] = nullptr;
        try {
            // std::addressof, NOT &: IWzFontPtr is a _com_ptr_t whose operator& is
            // overloaded to return Interface** for use as a COM out-param, so
            // `&m_pFont[i]` is an IWzFont** and does not compile here.
            PcCreateObject<IWzFontPtr>(L"Canvas#Font", m_pFont[i], nullptr);
            if (m_pFont[i]) {
                HRESULT hr = reinterpret_cast<HRESULT(__thiscall*)(IWzFont*, Ztl_bstr_t, unsigned long,
                    unsigned long, const Ztl_variant_t&)>(kAddr_SetFont)(
                    m_pFont[i], kFonts[i].face, kFonts[i].size, kFonts[i].col,
                    Ztl_variant_t(L""));
                // 0x0046341A raises a COM error instead of returning a failure, so the catch below
                // is what actually handles it; this check is only a belt-and-braces guard.
                if (FAILED(hr)) m_pFont[i] = nullptr;
            }
        } catch (...) { m_pFont[i] = nullptr; }
    }
}

CUICashShop::CUICashShop(int nLeft, int nTop)
    : m_screenX(nLeft), m_screenY(nTop),
      m_nTab(0), m_nCat(0), m_nSelItemId(0), m_nScroll(0),
      m_bDragging(0), m_nDragAnchorX(0), m_nDragAnchorY(0),
      m_nClosePressed(0), m_nCloseHover(0), m_nBuyPressed(0), m_nBuyHover(0),
      m_nHoverCell(-1), m_bSbDrag(0), m_nSbGrabDY(0), m_pCurrentStage(nullptr),
      m_pAvatar(nullptr), m_hAvatarLook(0), m_bAvatarDirty(true),
      m_nCartCount(0), m_nSentCount(0),
      m_nCartHover(-1), m_nBuyCartPressed(0), m_nBuyCartHover(0),
      m_bFocused(0), m_keyL(false), m_keyR(false), m_keyU(false), m_keyD(false),
      m_avX(static_cast<float>(kAvatarHomeX)), m_avY(static_cast<float>(kFloorY)),
      m_velX(0.0f), m_velY(0.0f), m_bAirborne(false), m_bFacingLeft(true), m_bOnLadder(false),
      m_bAttackReq(false), m_tLastStep(GetTickCount()),
      m_nLastMA(-1), m_nLastCode(kNoActionCode), m_nPendingEmotion(kNoEmotion),
      m_nSearchLen(0), m_bSearchActive(false),
      m_nPrevBg(s_nPrevBg), m_pEffectLayer(nullptr), m_nEffectItem(0),
      m_nEffectMA(-1), m_nEffectCode(kNoActionCode),
      m_bInStack(false), m_bTtInit(false), m_nTtItemId(0) {
    memset(m_ttBuf, 0, sizeof(m_ttBuf));
    m_ttRef[0] = m_ttRef[1] = nullptr;
    for (auto& id : m_cellIds) id = 0;
    m_avatarRef[0] = m_avatarRef[1] = 0;
    for (auto& e : m_cart) { e.itemId = 0; e.price = 0; }
    for (auto& s : m_sentIds) s = 0;
    m_szSearch[0] = 0;
    ms_pInstance = this;

    VerifySwingCode();

    LoadSprites();
    // bSetFocus = 1: the playground is live the moment the window opens, so the
    // arrow keys drive the preview rather than the real character straight away.
    CWnd::CreateWnd(this, nLeft, nTop, kWndW, kWndH, 10, 1, nullptr, 1);
    // Join the Esc close stack, as stock windows do in OnCreate.
    m_bInStack = true;
    StackRaise();
    play_ui_sound(L"MenuUp");
    MakeFonts();

    m_pCurrentStage = SehCurrentStage();
    m_nCat = CatIndexOfRow(0);
    SendRequestCatalog();
}

// ---------------------------------------------------------------------------
void CUICashShop::SelectTab(int i) {
    if (i < 0 || i >= kTabCount || i == m_nTab) return;
    m_nTab = i;
    m_nCat = CatIndexOfRow(0);
    m_nScroll = 0;
    HideItemTip();
    m_nSelItemId = 0;
    m_bAvatarDirty = true;
    SearchClear();              // a different list makes the old filter meaningless
    EnsureCategoryRequested();
    play_ui_sound(L"Tab");
    InvalidateRect(nullptr);
}

void CUICashShop::SelectCat(int absIndex) {
    if (absIndex < 0 || absIndex >= kCatCount || absIndex == m_nCat) return;
    m_nCat = absIndex;
    m_nScroll = 0;
    HideItemTip();
    m_nSelItemId = 0;
    m_bAvatarDirty = true;
    SearchClear();
    EnsureCategoryRequested();
    play_ui_sound(L"BtMouseClick");
    InvalidateRect(nullptr);
}

void CUICashShop::SelectItem(int itemId) {
    if (m_nSelItemId == itemId) return;
    m_nSelItemId = itemId;      // remembered, so BuildLook needs no catalogue lookup
    m_bAvatarDirty = true;      // the look itself is resolved from the cart plus the selection
    // A Facial Expression cannot be worn, so selecting one has to DO something or the
    // whole category looks broken. Queued rather than struck: see m_nPendingEmotion.
    const int emo = EmotionOfCashItem(itemId);
    if (emo != kNoEmotion) m_nPendingEmotion = emo;
    play_ui_sound(L"BtMouseClick");
    InvalidateRect(nullptr);
}

// One frame of the playground. Runs whether or not the window has focus: without
// focus the keys are all released, so the avatar simply falls to the floor and idles.
// Push a pose onto the LIVE avatar. Both halves are edge-guarded so a held key does
// not rebuild the action layers every frame.
//
// The override is STICKY, and that is the trap here: CAvatar::SetMoveAction only clears
// CAvatar+0x4EC for move action 9 (`and eax,0xFFFFFFFE; cmp eax,0x12; jne` at
// 0x00452111 skips the `or [esi+0x4EC],-1` for everything else), so LEAVING the attack
// has to write -1 back explicitly or the avatar stays stuck in the swing forever.
void CUICashShop::ApplyPose(const Pose& p) {
    if (!m_pAvatar) return;
    const int nMA = PackMoveAction(p.moveAction, m_bFacingLeft);
    const bool bMA   = (nMA != m_nLastMA);
    const bool bCode = (p.actionCode != m_nLastCode);
    if (!bMA && !bCode) return;

    if (bMA) {
        SehSetMoveAction(m_pAvatar, nMA);
        m_nLastMA = nMA;
    }
    // AFTER the move action, never before: SetMoveAction rebuilds over the override.
    if (bCode) {
        SehSetActionCode(m_pAvatar, p.actionCode);
        m_nLastCode = p.actionCode;
    }
}

void CUICashShop::StepPlayground(DWORD tNow) {
    if (!m_pAvatar) return;

    DWORD dt = tNow - m_tLastStep;
    m_tLastStep = tNow;
    if (dt == 0) return;
    if (dt > 100) dt = 100;                 // a stall must not teleport the avatar
    const float s = static_cast<float>(dt) / 1000.0f;

    // --- the ladder --------------------------------------------------------
    // Grab it by standing in the painted rungs and pressing Up. The avatar snaps to the
    // ladder's centre line, because a climb that is two pixels off the art reads as broken.
    if (!m_bOnLadder && !m_bAirborne && m_keyU &&
        m_avX >= static_cast<float>(LadderLeft()) && m_avX <= static_cast<float>(LadderRight())) {
        m_bOnLadder = true;
        m_avX = static_cast<float>(LadderX());
        m_velX = 0.0f;                          // the rungs hold you; no sliding off them
        m_velY = 0.0f;
    }
    if (m_bOnLadder) {
        if (m_keyU) m_avY -= kClimbPxPerSec * s;
        if (m_keyD) m_avY += kClimbPxPerSec * s;
        if (m_avY < static_cast<float>(kLadderTopY)) m_avY = static_cast<float>(kLadderTopY);
        if (m_avY >= static_cast<float>(kFloorY)) {          // climbed back down
            m_avY = static_cast<float>(kFloorY);
            m_bOnLadder = false;
        }
        if (m_bOnLadder) {
            // No gravity, no walking and no swinging while on the rungs -- the same as the
            // field, and it keeps the back view steady while you look at it.
            ApplyPose(kPoseLadder);
            m_bAttackReq = false;
            MoveAvatar(m_pAvatar, static_cast<int>(m_avX), static_cast<int>(m_avY));
            return;
        }
    }

    // "Attacking" is whatever the CLIENT says. The live +0x4EC override is set while the
    // swing plays and CAvatar::Update writes -1 back the moment it runs off its last
    // frame, so the animation always plays exactly once at its real WZ length and this
    // window never has to time it.
    const bool bAttacking = m_bAttackReq || (SehReadActionCode(m_pAvatar) != kNoActionCode);

    // Horizontal, integrated rather than assigned. Attacking takes the input away but NOT
    // the velocity, so a swing mid-stride slides to a halt instead of stopping dead.
    int dir = 0;
    if (!bAttacking) {
        if (m_keyL && !m_keyR) dir = -1;
        else if (m_keyR && !m_keyL) dir = 1;
    }
    if (dir) {
        // Facing follows the INPUT, not the velocity: turning around should look immediate
        // even while the old momentum is still carrying the avatar the other way.
        m_bFacingLeft = (dir < 0);
        m_velX += dir * kWalkAccel * s;
        if (m_velX >  kWalkPxPerSec) m_velX =  kWalkPxPerSec;
        if (m_velX < -kWalkPxPerSec) m_velX = -kWalkPxPerSec;
    } else if (!m_bAirborne) {
        // Drag is GROUND ONLY. That single asymmetry is most of what "momentum" means here:
        // let go mid-jump and the avatar keeps travelling until it lands, the way it does in
        // the field, instead of stopping in mid-air.
        const float d = kWalkDrag * s;
        if (m_velX > 0.0f)      m_velX = (m_velX >  d) ? m_velX - d : 0.0f;
        else if (m_velX < 0.0f) m_velX = (m_velX < -d) ? m_velX + d : 0.0f;
    }
    if (m_velX != 0.0f) {
        m_avX += m_velX * s;
        const float lo = static_cast<float>(kPrevX + kWalkMargin);
        const float hi = static_cast<float>(kPrevX + kPrevW - kWalkMargin);
        // Hitting the edge KILLS the velocity. Without this the avatar keeps its stored
        // speed against the wall and shoots off the moment it is nudged away from it.
        if (m_avX < lo) { m_avX = lo; m_velX = 0.0f; }
        if (m_avX > hi) { m_avX = hi; m_velX = 0.0f; }
    }
    const bool bMoving = (m_velX > kStopEps) || (m_velX < -kStopEps);

    // Vertical. Terminal velocity is clamped to the client's own fallSpeed so a long
    // drop cannot outrun the floor test between two frames.
    if (m_bAirborne) {
        m_velY += kGravity * s;
        if (m_velY > kFallSpeedMax) m_velY = kFallSpeedMax;
        m_avY  += m_velY * s;
        if (m_avY >= static_cast<float>(kFloorY)) {
            m_avY = static_cast<float>(kFloorY);
            m_velY = 0.0f;
            m_bAirborne = false;
        }
    }

    // Pick the pose. Priority mirrors the field: airborne beats attacking beats
    // walking beats prone beats idle.
    Pose pose;
    if (m_bAirborne)     pose = kPoseJump;
    else if (bAttacking) pose = PoseAttack();
    else if (bMoving)    pose = kPoseWalk;   // still sliding = still walking
    else if (m_keyD)     pose = kPoseProne;
    else                 pose = kPoseStand;

    ApplyPose(pose);
    // Consumed either way: an attack pressed mid-jump is dropped rather than queued, the
    // same as the field, where the jump owns the avatar until it lands.
    m_bAttackReq = false;
    MoveAvatar(m_pAvatar, static_cast<int>(m_avX), static_cast<int>(m_avY));
}

// The look the preview should be wearing: every cart item in add order, then the
// currently selected item on top. That ordering is what makes a single click a
// non-committal try-on and a double click a commitment -- the selection always wins the
// slot it shares with a cart item, and dropping the selection reveals the cart again.
void CUICashShop::BuildLook(LookOverride& out) const {
    for (int i = 0; i < m_nCartCount; ++i) ApplyItemToLook(out, m_cart[i].itemId);
    if (m_nSelItemId && CartIndexOfItem(m_nSelItemId) < 0) {
        ApplyItemToLook(out, m_nSelItemId);
    }
    // Last, once slot 5 has its final occupant.
    SettleOverall(out);
}

// The cart carries its own prices, so this needs no catalogue and no lock -- which also
// means a cart item stays priced correctly after its category has been evicted or was never
// loaded in this session.
int CUICashShop::CartTotalPrice() const {
    int total = 0;
    for (int i = 0; i < m_nCartCount; ++i) total += m_cart[i].price;
    return total;
}

// FNV-1a over the RESOLVED look, so the dirty test notices a cart change, a selection
// change, and a cart item being displaced out of a shared slot -- none of which a single
// remembered item id could distinguish.
unsigned int LookHash(const LookOverride& lk) {
    unsigned int h = 2166136261u;
    auto mix = [&h](int v) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(&v);
        for (int i = 0; i < 4; ++i) { h ^= p[i]; h *= 16777619u; }
    };
    mix(lk.hair); mix(lk.face); mix(lk.sticker);
    for (int s = 0; s <= kMaxAvatarSlot; ++s) mix(lk.slot[s]);
    return h;
}

void CUICashShop::CartToggle(int itemId, int price) {
    if (itemId == 0) return;
    const int at = CartIndexOfItem(itemId);
    if (at >= 0) {
        for (int i = at; i + 1 < m_nCartCount; ++i) m_cart[i] = m_cart[i + 1];
        --m_nCartCount;
    } else {
        if (m_nCartCount >= kCartMax) {
            _snprintf(g_szStatus, sizeof(g_szStatus),
                      "The cart holds %d items.", kCartMax);
            g_szStatus[sizeof(g_szStatus) - 1] = 0;
            play_ui_sound(L"BtMouseClick");
            InvalidateRect(nullptr);
            return;
        }
        m_cart[m_nCartCount].itemId = itemId;
        m_cart[m_nCartCount].price = price;
        ++m_nCartCount;
    }
    play_ui_sound(L"DragEnd");
    m_bAvatarDirty = true;
    InvalidateRect(nullptr);
}

// The selected item's effect animation, (re)built when the item or the pose changes.
// Per-action effects have a node per action name, so a pose change really is a rebuild.
void CUICashShop::RefreshEffect() {
    if (!m_pAvatar) { ReleaseEffect(); return; }

    const int want = (m_nSelItemId && IsCashEffectItem(m_nSelItemId)) ? m_nSelItemId : 0;
    if (want == 0) { ReleaseEffect(); return; }

    // The memo is the WHOLE test -- deliberately not `m_pEffectLayer && ...`. A null layer
    // is a legitimate cached answer of "this WZ shape is not one we play", and requiring a
    // live layer here meant the follow-trail items (5010022/024/041/052), 5010044's
    // canvas-less spectrum, and any per-action item on a pose it has no art for re-ran
    // three GetObjectA path probes EVERY frame for as long as they stayed selected.
    // ReleaseEffect resets the triple, so a cleared cache still forces the first build.
    if (m_nEffectItem == want &&
        m_nEffectMA == m_nLastMA && m_nEffectCode == m_nLastCode) {
        return;
    }

    ReleaseEffect();
    m_pEffectLayer = CreateEffectLayer(m_pAvatar, want,
                                       ActionNameForPose(m_nLastMA, m_nLastCode));
    // Recorded even when the build FAILED, so an item whose shape this window does not
    // play is attempted once per pose rather than once per frame.
    m_nEffectItem = want;
    m_nEffectMA   = m_nLastMA;
    m_nEffectCode = m_nLastCode;
}

void CUICashShop::RebuildAvatar() {
    // RESOLVE FIRST, COMPARE, and only then tear anything down.
    //
    // m_bAvatarDirty only means "something the look might depend on changed" -- a tab
    // click, a category click, a catalog push, a buy reply. Most of those do not change
    // the resolved look at all, and rebuilding anyway throws away state that lives ONLY on
    // the CAvatar object: the walk cycle's frame position, an in-flight swing (whose sole
    // record is the +0x4EC override), and a held facial expression (CAvatar+0x48C, which
    // Init resets by installing emotion 0 as its 9th argument). Pressing BUY during a
    // 5-second expression used to cancel it, because the reply raises g_bCatalogDirty.
    LookOverride look;
    BuildLook(look);
    const unsigned int h = LookHash(look);
    if (m_pAvatar && h == m_hAvatarLook) return;

    ReleaseEffect();                       // BEFORE the avatar: it holds the avatar's vectors
    SehReleaseAvatar(m_avatarRef);
    m_avatarRef[0] = m_avatarRef[1] = 0;
    m_pAvatar = nullptr;

    void* charData = SehCharacterData();
    if (!charData) return;
    IWzGr2DLayer* pLayer = m_pLayer.GetInterfacePtr();
    if (!pLayer) return;

    m_hAvatarLook = h;

    // Rebuilt at wherever it currently stands, so trying on an item does not
    // teleport the avatar back to the middle of the pane mid-walk.
    const int nMA = PackMoveAction(kPoseStand.moveAction, m_bFacingLeft);
    // CAvatar::Init builds the first action layer, so the tint scope has to be armed
    // here too -- otherwise the avatar appears untinted until the first pose change.
    // Same reason as ApplyPose: this avatar is not CUserLocal's.
    SehBuildAvatar(m_avatarRef, charData, pLayer,
                   static_cast<int>(m_avX), static_cast<int>(m_avY),
                   &look, &m_pAvatar, nMA, kNoActionCode);
    // Mirrors the state the fresh avatar is actually in, so the next ApplyPose only
    // pushes what really changed.
    m_nLastMA   = nMA;
    m_nLastCode = kNoActionCode;
}

// ---------------------------------------------------------------------------
void CUICashShop::Update() {
    // Auto-close on a stage change (field warp, character select), the same poll
    // sibling windows use.
    if (SehCurrentStage() != m_pCurrentStage) { Destroy(); return; }

    if (g_bCartBought.exchange(false)) {
        // Bought and delivered. Only the serials that were actually SENT are retired --
        // anything double-clicked in while the request was in flight was never bought and
        // must survive. The preview then drops back to the character's real look, which is
        // honest: the items are in the inventory now, not worn.
        CartRetireSent();
    }

    if (g_bCatalogDirty.exchange(false)) {
        ClampScroll();
        HideItemTip();
        m_bAvatarDirty = true;
        InvalidateRect(nullptr);
    }

    if (g_bCashDirty.exchange(false)) {
        InvalidateRect(nullptr);
    }

    // The index arrives AFTER the constructor has run, so the opening category cannot be
    // requested there. Idempotent -- g_loaded/g_pending make it two set lookups once the
    // category is in flight -- and it is also what fetches a category whose request was
    // raised before the index had told us the category exists.
    EnsureCategoryRequested();

    if (m_bAvatarDirty) {
        m_bAvatarDirty = false;
        RebuildAvatar();
        InvalidateRect(nullptr);
    }

    // AFTER the rebuild, never before. CAvatar::Init installs emotion 0 (its 9th argument),
    // so an expression struck earlier in the frame would be thrown away here.
    // The five-second hold is then the CLIENT'S: SetEmotion stamps now + duration into
    // CAvatar+0x48C and the tick below expires it back to the normal face on its own.
    if (m_nPendingEmotion != kNoEmotion && m_pAvatar) {
        SehSetEmotion(m_pAvatar, m_nPendingEmotion, kExpressionHoldMs);
        m_nPendingEmotion = kNoEmotion;
    }

    StepPlayground(GetTickCount());

    SehUpdateAvatar(m_pAvatar);

    // Resync the override edge-guard from the object. CAvatar::Update clears +0x4EC itself
    // when the swing ends, so a remembered copy would go stale and ApplyPose would stop
    // pushing poses.
    m_nLastCode = SehReadActionCode(m_pAvatar);

    // After the pose is settled and the override resynced, so a per-action effect is built
    // for the pose actually on screen.
    RefreshEffect();

    // The scrollbar thumb, for the same reason as the title-bar drag below: a fast drag
    // takes the cursor outside the window and OnMouseMove simply stops arriving.
    if (m_bSbDrag) {
        if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
            m_bSbDrag = 0;
        } else {
            POINT sp;
            if (GetAbsCursor(sp)) DragThumbTo(sp.y - m_screenY);
        }
    }

    // The title-bar drag has to keep tracking when the cursor leaves the window;
    // driving it from OnMouseMove made the drag freeze.
    if (m_bDragging) {
        if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
            m_bDragging = 0;
            s_bSavedPos = true; s_savedX = m_screenX; s_savedY = m_screenY;
        } else {
            POINT sp;
            if (GetAbsCursor(sp)) {
                int nx = sp.x - m_nDragAnchorX, ny = sp.y - m_nDragAnchorY;
                ClampToScreen(nx, ny);   // a drag must not be able to strand the window
                if (nx != m_screenX || ny != m_screenY) {
                    m_screenX = nx; m_screenY = ny;
                    MoveWnd(m_screenX, m_screenY);
                }
            }
        }
    }
    InvalidateRect(nullptr);
}

void CUICashShop::OnDestroy() {
    HideItemTip();
    // Leave the Esc stack BEFORE CWnd::OnDestroy, as stock OnDestroy does; a stale entry
    // would be handed to TryCloseUI after this object is freed.
    StackRemove();
    ReleaseEffect();                       // BEFORE the avatar: it holds the avatar's vectors
    SehReleaseAvatar(m_avatarRef);
    m_avatarRef[0] = m_avatarRef[1] = 0;
    m_pAvatar = nullptr;
    s_bSavedPos = true; s_savedX = m_screenX; s_savedY = m_screenY;
    if (ms_pInstance == this) ms_pInstance = nullptr;
    play_ui_sound(L"MenuDown");
    CWnd::OnDestroy();
    // AFTER CWnd::OnDestroy, because the unregister inside it is what nulls
    // CWndMan::m_pFocus. Without this the player stays frozen after closing the
    // window until some other key or click happens to re-focus the manager.
    RestoreFocusToWndMan();
}

// ---------------------------------------------------------------------------
// The stock frame stack, outside in: a soft shadow pixel, a white highlight, the
// #557799 line, then the body. Measured off Skill/backgrnd rather than guessed.
void CUICashShop::DrawFrame(IWzCanvasPtr c) {
    // THE PLATE. One blit replaces the body, all three frame rings, the caption bevel, both
    // tab bands, the red rule, the section-head strips, every well outline and the footer band.
    // Its own alpha carries the rounded corners, so nothing is cut here.
    if (m_pWndBg) {
        BlitA(c, m_pWndBg, 0, 0);
    } else {
        // No plate in the WZ: draw enough chrome from primitives that the window is still
        // usable rather than a transparent hole. Not the full old stack -- just the body, the
        // frame rings and the two bands the eye needs to find the tabs.
        FillRounded(c, 0, 0, kWndW - 1, kWndH - 1, kColPlate);
        RoundRing(c, 0, 0, kWndW - 1, kWndH - 1, kColShadow);
        RoundRing(c, 1, 1, kWndW - 2, kWndH - 2, kColWhite);
        RoundRing(c, 2, 2, kWndW - 3, kWndH - 4, kColFrame);
        Fill(c, kFrameX, kBandY, kFrameW, kBandH, kColShade);
        Fill(c, kFrameX, kRedY, kFrameW, 1, kColRedTop);
        Fill(c, kFrameX, kRedY + 1, kFrameW, kRedH - 1, kColRed);
        Fill(c, kFrameX, kSubY, kFrameW, kSubH - 1, kColAccent);
        Fill(c, kFrameX, kBarY, kFrameW, kBarH, kColBand);
    }

    // "CASH SHOP" is baked into the plate; nothing is lettered here any more.

    const int state = m_nClosePressed ? 1 : (m_nCloseHover ? 3 : 0);
    if (m_pBtClose[state]) {
        BlitA(c, m_pBtClose[state], kCloseX, kCloseY);
    } else {
        // No art: draw the X rather than leave a dead corner.
        for (int i = 0; i < kCloseSize; ++i) {
            Fill(c, kCloseX + i, kCloseY + i, 2, 1, kColText);
            Fill(c, kCloseX + (kCloseSize - 1 - i), kCloseY + i, 2, 1, kColText);
        }
    }
}

void CUICashShop::DrawTabs(IWzCanvasPtr c) {
    // The band and its red rule are PAINTED into the plate; only the tabs themselves change
    // with the selection, so only they are drawn.

    for (int i = 0; i < kTabCount; ++i) {
        const int x = TabX(i);
        const bool sel = (i == m_nTab);
        DrawTab(c, x, kTabY, kTabW, kTabH, sel, true);   // topOnly: joined to the body
        // The +1 belongs to the UNSELECTED tab only: its face is inset 3px from the top
        // (DrawTab fills at y+3), where the selected tab's face is the full height. A flat
        // +1 put the selected label a pixel below its neighbours, side by side.
        StrCenter(c, sel ? kF_Text : kF_Dim, x + kTabW / 2,
                  kTabY + 3 + (sel ? 0 : 1), kTabs[i].name);
    }

}

// One category tab, drawn from the INVENTORY's own 3-slice: left cap, the 1px fill column
// repeated across the middle, then the angled right cap. Falls back to the drawn tab if the
// art is missing, so a stripped WZ still gets a tab strip rather than a bare band.
void CUICashShop::DrawSubTab(IWzCanvasPtr c, int x, int y, int w, bool sel) {
    const int i = sel ? 1 : 0;
    const int h = kSubArtH[i];
    IWzCanvasPtr T = m_pSubTab[i];
    if (!T || w < kSubCapL + kSubCapR + 2) {
        DrawTab(c, x, y, w, h, sel, true);
        return;
    }
    try {
        // The three slices come from ONE canvas at three source offsets; the plates carry
        // no baked background, so nothing is cropped vertically.
        // (dstX, dstY, src, alpha, dstW, dstH, srcX, srcY, srcW, srcH)
        c->CopyEx(x, y, T, CANVAS_ALPHATYPE::CA_OVERWRITE, kSubCapL, h, 0, 0, kSubCapL, h);
        const int midW = w - kSubCapL - kSubCapR;
        if (midW > 0) {
            c->CopyEx(x + kSubCapL, y, T, CANVAS_ALPHATYPE::CA_OVERWRITE,
                      midW, h, kSubFillX, 0, 1, h);
        }
        c->CopyEx(x + w - kSubCapR, y, T, CANVAS_ALPHATYPE::CA_OVERWRITE,
                  kSubCapR, h, kSubArtW - kSubCapR, 0, kSubCapR, h);
    } catch (...) {}
}

// TIER 2, the category strip. See kSubY for why the band underneath carries the tier.
void CUICashShop::DrawCategories(IWzCanvasPtr c) {
    // The band is painted into the plate; only the tabs move.

    const int rows = CatRowCount();
    for (int r = 0; r < rows; ++r) {
        const int idx = CatIndexOfRow(r);
        if (idx < 0) continue;
        const bool sel = (idx == m_nCat);
        const int x = SubTabX(r);
        const int w = SubTabW(r);
        if (w <= 0) continue;
        // Bottom-aligned, so the selected tab's extra row shows as a POP at its top edge
        // rather than as a tab hanging below its neighbours.
        const int y = kSubBot - kSubArtH[sel ? 1 : 0];
        DrawSubTab(c, x, y, w, sel);

        char nm[40];
        _snprintf(nm, sizeof(nm), "%s", kCats[idx].name);
        nm[sizeof(nm) - 1] = 0;
        // WHITE on the selected plate, dark on the unselected one: the bodies are #EE6688
        // rose and #BBBBBB grey, and the inventory's own baked word glyphs are white. Dark
        // text on the rose measures 2.6:1 and is the one combination that fails here.
        const int fi = sel ? kF_SubSel : kF_Text;
        Clip(fi, nm, w - 10);
        // The plates are symmetric, so the label centres on the full width.
        StrCenter(c, fi, x + w / 2, y + (kSubArtH[sel ? 1 : 0] - 12) / 2, nm);
    }
}

void CUICashShop::DrawGrid(IWzCanvasPtr c, const std::vector<Entry>& vis) {
    // NO BOXED PANE. Stock windows have essentially no interior borders, so the grid is the
    // white plate itself, opened by a flat header strip and its red rule.
    const int paneH = kPaneBottom - kPaneTop;
    const CatDef* cd = (m_nCat >= 0 && m_nCat < kCatCount) ? &kCats[m_nCat] : nullptr;
    DrawSectionHead(c, kGridX, kPaneTop, kGridW, cd ? cd->name : "ITEMS", "");

    // The count sits left of the box, and reports what is ON SCREEN -- so while a filter is
    // typed it counts the matches, which is the number that is actually useful.
    char head[40];
    if (m_nSearchLen) {
        _snprintf(head, sizeof(head), "%d match%s", static_cast<int>(vis.size()),
                  (vis.size() == 1) ? "" : "es");
    } else {
        _snprintf(head, sizeof(head), "%d item%s", static_cast<int>(vis.size()),
                  (vis.size() == 1) ? "" : "s");
    }
    head[sizeof(head) - 1] = 0;
    StrRight(c, kF_HeadR, kSearchX - 8, kPaneTop + (kHeadH - 12) / 2, head);

    DrawSunken(c, kSearchX, kSearchY, kSearchW, kSearchH);
    if (m_bSearchActive) Border(c, kSearchX, kSearchY, kSearchW, kSearchH, kColFrame);
    if (m_nSearchLen) {
        char shown[kSearchMax + 2];
        _snprintf(shown, sizeof(shown), "%s%s", m_szSearch, m_bSearchActive ? "_" : "");
        shown[sizeof(shown) - 1] = 0;
        Str(c, kF_Text, kSearchX + 5, kSearchY + (kSearchH - 12) / 2, shown);
    } else {
        Str(c, kF_Dim, kSearchX + 5, kSearchY + (kSearchH - 12) / 2,
            m_bSearchActive ? "_" : "Search this category...");
    }

    const int first = m_nScroll * kCols;
    for (int r = 0; r < kRows; ++r) {
        for (int col = 0; col < kCols; ++col) {
            const int cell = r * kCols + col;
            const int idx  = first + cell;
            const int x = kCellX0 + col * kCellW;
            const int y = kCellY0 + r * kCellH;
            const bool has = idx >= 0 && idx < static_cast<int>(vis.size());
            const bool sel = has && vis[idx].itemId == m_nSelItemId;
            m_cellIds[cell] = has ? vis[idx].itemId : 0;   // what OnMouseMove tooltips

            // Flat faces, the stock list idiom: Skill/skill0 for a row and skill1 for the
            // selected one, with no border on either. What replaces the border is a 1px
            // separator on the cell's bottom and right edges -- without it a price floats
            // ambiguously between the icon above it and the icon below.
            // The cell face, its separators and the icon well are all painted into the plate.
            // Only the two states that CHANGE are drawn: the selection, and the hover frame.
            if (sel) {
                Fill(c, x, y, kCellW - 1, kCellH - 1, kColSelBg);
                Border(c, x, y, kCellW - 1, kCellH - 1, kColLine);
            } else if (has && m_nHoverCell == cell) {
                Border(c, x, y, kCellW - 1, kCellH - 1, kColLine);
            }

            // The icon well is the stock inventory cell, drawn on EVERY slot whether it
            // is filled or not: a grid of empty wells reads as "nothing in this
            // category", where bare plate reads as "the window failed to draw".
            const int bx = x + kCellBoxDX, by = y + kCellBoxDY;
            if (sel) DrawSunken(c, bx, by, kCellBox, kCellBox);   // repaint over the selection
            if (!has) continue;

            const Entry& e = vis[idx];
            IconInBox(c, e.itemId, bx, by, kCellBox);
            if (CartIndexOfItem(e.itemId) >= 0) {
                // Already in the cart: a corner flag on the well. A border would be lost
                // against the selection frame, and a tint would fight the icon.
                Fill(c, bx + kCellBox - 8, by, 8, 8, kColFrame);
                Fill(c, bx + kCellBox - 7, by + 1, 6, 6, kColAccent);
            }
            if (e.count > 1) {
                char q[12];
                _snprintf(q, sizeof(q), "x%d", e.count);
                q[sizeof(q) - 1] = 0;
                StrRight(c, sel ? kF_Sel : kF_Dim, bx + kCellBox, by + 21, q);
            }

            const int cx  = x + kCellW / 2;
            const int fi  = sel ? kF_Sel : kF_Text;
            const int maxw = kCellW - 12;

            // TWO LINES, broken on a space. One 79px line cut 89% of real catalogue names
            // mid-word ("Abyssal Aris", "Crunchy Fri"); two 86px lines carry 87% of them
            // whole, which is the entire reason the grid gave up a column.
            char nm[64];
            _snprintf(nm, sizeof(nm), "%s", e.name.c_str());
            nm[sizeof(nm) - 1] = 0;
            if (TextW(fi, nm) <= maxw) {
                StrCenter(c, fi, cx, by + kCellBox + 4, nm);
            } else {
                // The last space that still fits, so the break lands between words.
                int cut = -1;
                for (int k = 0; nm[k]; ++k) {
                    if (nm[k] != ' ') continue;
                    nm[k] = 0;
                    const bool fits = TextW(fi, nm) <= maxw;
                    nm[k] = ' ';
                    if (!fits) break;
                    cut = k;
                }
                char l1[64], l2[64];
                if (cut > 0) {
                    _snprintf(l1, sizeof(l1), "%.*s", cut, nm);
                    _snprintf(l2, sizeof(l2), "%s", nm + cut + 1);
                } else {
                    // One word longer than the cell: hard-clip it and carry the remainder.
                    _snprintf(l1, sizeof(l1), "%s", nm);
                    l1[sizeof(l1) - 1] = 0;
                    Clip(fi, l1, maxw);
                    _snprintf(l2, sizeof(l2), "%s", nm + strlen(l1));
                }
                l1[sizeof(l1) - 1] = 0;
                l2[sizeof(l2) - 1] = 0;
                Clip(fi, l2, maxw);
                StrCenter(c, fi, cx, by + kCellBox + 4, l1);
                StrCenter(c, fi, cx, by + kCellBox + 17, l2);
            }

            char pr[24];
            _snprintf(pr, sizeof(pr), "%d NX", e.price);
            pr[sizeof(pr) - 1] = 0;
            StrCenter(c, sel ? kF_Sel : kF_Price, cx, by + kCellBox + 31, pr);
        }
    }
    if (vis.empty()) {
        StrCenter(c, kF_Dim, kGridX + kGridW / 2, kPaneTop + paneH / 2 - 6,
                  CurrentCategoryLoaded() ? "There are no items in this category."
                                          : "Loading...");
    }
}

// The thumb's travel. The track runs BETWEEN the arrow caps, which is why the arrow
// height is subtracted at both ends rather than only at the top.
void CUICashShop::ThumbRect(RECT& out, int total) const {
    const int trackY = kScrollTop + kSbArrowH;
    const int trackH = (kScrollBot - kSbArrowH) - trackY;
    const int rows   = (total + kCols - 1) / kCols;
    int span = trackH;
    if (rows > kRows) {
        span = trackH * kRows / rows;
        if (span < kThumbMinH) span = kThumbMinH;
        if (span > trackH)     span = trackH;
    }
    int maxScroll = rows - kRows;
    if (maxScroll < 0) maxScroll = 0;
    const int travel = trackH - span;
    const int top = trackY + ((maxScroll > 0) ? (travel * m_nScroll / maxScroll) : 0);
    out.left = kScrollX; out.right = kScrollX + kScrollW;
    out.top  = top;      out.bottom = top + span;
}

void CUICashShop::DrawScrollbar(IWzCanvasPtr c, int total) {
    // Drawn ALWAYS, even when the grid fits: a bar that appears and vanishes as the
    // category changes is more distracting than a thumb that simply fills its track.
    auto stretchV = [&](IWzCanvasPtr sp, int y, int h) {
        if (sp && h > 0)
            try { c->CopyEx(kScrollX, y, sp, CANVAS_ALPHATYPE::CA_OVERWRITE,
                            kScrollW, h, 0, 0, 0, 0); } catch (...) {}
    };
    const int trackY = kScrollTop + kSbArrowH;
    const int trackH = (kScrollBot - kSbArrowH) - trackY;
    if (!m_pSbBase || !m_pSbThumb) {
        // No art: a plain sunken groove rather than a gap where the bar should be.
        DrawSunken(c, kScrollX, kScrollTop, kScrollW, kScrollBot - kScrollTop);
        return;
    }
    stretchV(m_pSbBase, trackY, trackH);
    BlitA(c, m_pSbPrev, kScrollX, kScrollTop);
    BlitA(c, m_pSbNext, kScrollX, kScrollBot - kSbArrowH);

    RECT th;
    ThumbRect(th, total);
    const int thH = th.bottom - th.top;
    if (thH < 2 * kSbThumbCap + 2) { stretchV(m_pSbThumb, th.top, thH); return; }
    try {
        c->CopyEx(kScrollX, th.top, m_pSbThumb, CANVAS_ALPHATYPE::CA_OVERWRITE,
                  kScrollW, kSbThumbCap, 0, 0, kScrollW, kSbThumbCap);
        c->CopyEx(kScrollX, th.top + kSbThumbCap, m_pSbThumb, CANVAS_ALPHATYPE::CA_OVERWRITE,
                  kScrollW, thH - 2 * kSbThumbCap, 0, kSbThumbCap,
                  kScrollW, kSbThumbSrcH - 2 * kSbThumbCap);
        c->CopyEx(kScrollX, th.bottom - kSbThumbCap, m_pSbThumb, CANVAS_ALPHATYPE::CA_OVERWRITE,
                  kScrollW, kSbThumbCap, 0, kSbThumbSrcH - kSbThumbCap, kScrollW, kSbThumbCap);
    } catch (...) {}
}

void CUICashShop::DrawPreview(IWzCanvasPtr c, const std::vector<Entry>& vis) {
    DrawSectionHead(c, kPrevX, kPaneTop, kPrevW, "PREVIEW", "");
    // The backdrop picker, parked in the header. Three tiny tabs rather than a cycle button
    // so the current one is visible at rest -- and because the ladder sits in a different
    // column in each, which matters the moment you want the back view.
    for (int i = 0; i < kBgCount; ++i) {
        const bool sel = (i == m_nPrevBg);
        const int x = BgTabX(i);
        if (sel) {
            Fill(c, x, kBgTabY, kBgTabW, kBgTabH, kColWhite);
            Border(c, x, kBgTabY, kBgTabW, kBgTabH, kColFrame);
        } else {
            FillV(c, x, kBgTabY, kBgTabW, kBgTabH, kColBtnFace, kColBtnFace2);
            Border(c, x, kBgTabY, kBgTabW, kBgTabH, kColAccent2);
        }
        char lbl[4];
        _snprintf(lbl, sizeof(lbl), "%d", i + 1);
        lbl[sizeof(lbl) - 1] = 0;
        StrCenter(c, sel ? kF_Text : kF_HeadR, x + kBgTabW / 2, kBgTabY + (kBgTabH - 12) / 2, lbl);
    }
    // The focus lamp. Centred on the same row as the backdrop tabs so the header's two live
    // elements share a baseline.
    {
        const int ly = kBgTabY + (kBgTabH - kLampSize) / 2;
        Fill(c, kLampX, ly, kLampSize, kLampSize,
             m_bFocused ? kColLampOn : kColLampOff);
        Border(c, kLampX - 1, ly - 1, kLampSize + 2, kLampSize + 2, kColLampEdge);
    }

    // The playground. The avatar draws itself onto this window's LAYER, which sits over
    // the canvas, so only the well behind it is painted here -- as a lit stage rather than
    // a white box, which also stops a pale character disappearing into the background.
    DrawSunken(c, kPrevX + 4, kPlayY, kPlayW, kPlayH);
    IWzCanvasPtr bg = m_pPrevBg[m_nPrevBg];
    if (bg) {
        // Source-cropped, never stretched: 212 wide art into a 182 well, taking the middle.
        // Args are (dstX, dstY, src, alpha, dstW, dstH, srcX, srcY, srcW, srcH).
        try {
            c->CopyEx(kPrevX + 4, kPlayY, bg, CANVAS_ALPHATYPE::CA_OVERWRITE,
                      kPlayW, kPlayH, kPrevArtSrcX, 0, kPlayW, kPlayH);
        } catch (...) {}
    } else {
        // No art: the lit stage it used to be, so the avatar still has something to stand on.
        FillV(c, kPrevX + 5, kPlayY + 1, kPlayW - 2, kPlayH - 2, kColStageTop, kColStageBot);
        Fill(c, kPrevX + 6, kFloorY + 1, kPlayW - 4, 1, kColFloor);
        Fill(c, kPrevX + 6, kFloorY + 2, kPlayW - 4, 3, kColFloorSh);
    }

    // The cart. Every well is drawn whether it is filled or not, so the strip reads as
    // "ten slots, three used" rather than as a gap.
    // Always the same shape: the fill level, plus the running total once there is one. The
    // total belongs on the cart rather than adrift in the footer band next to the SELECTED
    // item's price, where the two numbers read as one.
    char head[40];
    if (m_nCartCount == 0) {
        _snprintf(head, sizeof(head), "%d/%d", m_nCartCount, kCartMax);
    } else {
        _snprintf(head, sizeof(head), "%d/%d   %d NX",
                  m_nCartCount, kCartMax, CartTotalPrice());
    }
    head[sizeof(head) - 1] = 0;
    // A SHORT head: this labels a strip inside the column rather than opening a pane, so it
    // takes a plain 18px #CCCCCC band and a hairline. A full pane head would put a second
    // red rule halfway down the column, which reads as the window starting over.
    // "CART" is baked into the plate; only the fill count is drawn.
    StrRight(c, kF_HeadR, kPrevX + kPrevW - 7, kCartHeadY + (kCartHeadH - 12) / 2, head);
    for (int i = 0; i < kCartMax; ++i) {
        const int bx = kCartX0 + (i % kCartCols) * (kCartBox + kCartGap);
        const int by = kCartY + 2 + (i / kCartCols) * (kCartBox + kCartGap);
        if (i >= m_nCartCount) continue;
        IconInBox(c, m_cart[i].itemId, bx, by, kCartBox);
        // Hovering a filled well is the only hint that it can be removed, so it gets the
        // selection frame rather than the pale hairline an empty cell would take.
        if (m_nCartHover == i) Border(c, bx, by, kCartBox, kCartBox, kColFrame);
    }

    // CASH. The two captions and their recessed value fields are painted into the plate, so
    // only the numbers are drawn -- right-aligned into those fields, the way every stock
    // readout aligns a number.
    long long cash[3];
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        cash[0] = g_cash[0]; cash[1] = g_cash[1]; cash[2] = g_cash[2];
    }
    char v[40];
    FormatCashFit(v, sizeof(v), cash[0], kCashFieldW);
    StrRight(c, kF_Text, kCashNxRight, kCashValueY, v);
    _snprintf(v, sizeof(v), "%d", static_cast<int>(cash[1])); v[sizeof(v) - 1] = 0;
    StrRight(c, kF_Text, kCashMpRight, kCashValueY, v);
}

// THE BUY BAR, full width under both panes. Its furniture is the decoded stock footer band:
// a #557799 opening rule, a three-row lighter lip, then FLAT #99BBCC.
void CUICashShop::DrawBuyBar(IWzCanvasPtr c) {
    // The band is painted into the plate.

    // The status message takes the space the item info used to hold, clipped short of the
    // buttons rather than given a fixed reservation now that nothing competes with it.
    if (g_szStatus[0]) {
        char st[96];
        _snprintf(st, sizeof(st), "%s", g_szStatus);
        st[sizeof(st) - 1] = 0;
        Clip(kF_Sel, st, kBuyX - (kFrameX + 11) - 12);
        Str(c, kF_Sel, kFrameX + 11, kStatusY, st);
    }

    DrawVanillaButton(c, kBuyX, kBtnY, kBtnW, "BUY",
                      m_nSelItemId != 0, m_nBuyPressed != 0, m_nBuyHover != 0, m_pBtBuy);
    DrawVanillaButton(c, kCartBuyX, kBtnY, kBtnW, "BUY CART",
                      m_nCartCount > 0, m_nBuyCartPressed != 0, m_nBuyCartHover != 0, m_pBtCart);
}

// Background to foreground, one pass, no dirty regions.
void CUICashShop::Draw(const RECT* pRect) {
    CWnd::Draw(pRect);
    IWzCanvasPtr c = GetCanvas();
    if (!c) return;

    // Collected ONCE per frame: the grid, the scrollbar and the footer all need the same
    // view, and taking g_mtx three times would let the receive thread change the catalog
    // between them and draw a frame that disagrees with itself.
    std::vector<Entry> vis;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        CollectLocked(vis);
    }

    DrawFrame(c);
    DrawTabs(c);
    DrawCategories(c);
    DrawGrid(c, vis);
    DrawScrollbar(c, static_cast<int>(vis.size()));
    DrawPreview(c, vis);
    DrawBuyBar(c);        // buttons and the status line; no selection info
}

// ---------------------------------------------------------------------------
static bool In(int x, int y, int l, int t, int w, int h) {
    return x >= l && x < l + w && y >= t && y < t + h;
}

// Scroll so that the grabbed point of the thumb follows the cursor. Driven from Update
// rather than OnMouseMove for the same reason the title-bar drag is: the cursor leaves
// the window during a fast drag and OnMouseMove stops arriving, which strands the thumb.
void CUICashShop::DragThumbTo(int localY) {
    const int total = VisibleCount();
    const int rows  = (total + kCols - 1) / kCols;
    const int maxScroll = rows - kRows;
    if (maxScroll <= 0) { m_nScroll = 0; return; }

    const int trackY = kScrollTop + kSbArrowH;
    const int trackH = (kScrollBot - kSbArrowH) - trackY;
    RECT th;
    ThumbRect(th, total);
    const int travel = trackH - (th.bottom - th.top);
    if (travel <= 0) return;

    int rel = localY - m_nSbGrabDY - trackY;
    if (rel < 0)      rel = 0;
    if (rel > travel) rel = travel;
    m_nScroll = (rel * maxScroll + travel / 2) / travel;
    ClampScroll();
}

int CUICashShop::OnMouseMove(int rx, int ry) {
    m_nCloseHover   = In(rx, ry, kCloseX, kCloseY, kCloseSize, kCloseSize) ? 1 : 0;
    m_nBuyHover     = In(rx, ry, kBuyX, kBtnY, kBtnW, kBtnH) ? 1 : 0;
    m_nBuyCartHover = In(rx, ry, kCartBuyX, kBtnY, kBtnW, kBtnH) ? 1 : 0;

    m_nCartHover = -1;
    for (int i = 0; i < m_nCartCount; ++i) {
        const int bx = kCartX0 + (i % kCartCols) * (kCartBox + kCartGap);
        const int by = kCartY + 2 + (i / kCartCols) * (kCartBox + kCartGap);
        if (In(rx, ry, bx, by, kCartBox, kCartBox)) { m_nCartHover = i; break; }
    }

    int hover = -1;
    for (int r = 0; r < kRows && hover < 0; ++r)
        for (int col = 0; col < kCols; ++col)
            if (In(rx, ry, kCellX0 + col * kCellW, kCellY0 + r * kCellH, kCellW - 1, kCellH - 1)) {
                hover = r * kCols + col; break;
            }
    m_nHoverCell = hover;

    // The item under the cursor: a grid cell, or a cart well. Re-shown on every move, the
    // stock way, so the tooltip follows the cursor; cleared the moment it leaves the icon.
    int tipItem = 0;
    if (m_nCartHover >= 0 && m_nCartHover < m_nCartCount) {
        tipItem = m_cart[m_nCartHover].itemId;
    } else if (hover >= 0 && !m_bDragging) {
        const int col = hover % kCols, r = hover / kCols;
        const int bx = kCellX0 + col * kCellW + kCellBoxDX;
        const int by = kCellY0 + r * kCellH + kCellBoxDY;
        if (In(rx, ry, bx, by, kCellBox, kCellBox)) tipItem = m_cellIds[hover];
    }
    if (tipItem > 0) ShowItemTip(tipItem, rx, ry);
    else             HideItemTip();

    InvalidateRect(nullptr);
    return 1;
}

// ---------------------------------------------------------------------------
// Esc stack
void CUICashShop::StackRemove() {
    void* ctx = *reinterpret_cast<void**>(kAddr_CWvsContext);
    if (!ctx) return;
    __try {
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kAddr_RemoveFromStackForTab)(
            ctx, static_cast<CWnd*>(this));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Moves this window to the top of the stack (remove is a no-op when absent).
void CUICashShop::StackRaise() {
    void* ctx = *reinterpret_cast<void**>(kAddr_CWvsContext);
    if (!ctx) return;
    __try {
        void* top = reinterpret_cast<void*(__thiscall*)(void*)>(kAddr_GetTopStackForTab)(ctx);
        if (top == static_cast<CWnd*>(this)) return;
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kAddr_RemoveFromStackForTab)(
            ctx, static_cast<CWnd*>(this));
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kAddr_PushStackForTab)(
            ctx, static_cast<CWnd*>(this));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------
// Item tooltip
void CUICashShop::ReleaseTipItem() {
    // 0x00428A50 dereferences p unconditionally, so an empty ref must never reach it.
    if (m_ttRef[1]) {
        try {
            reinterpret_cast<void(__thiscall*)(void*, int)>(kAddr_ZRefItemSlot_Release)(m_ttRef, 0);
        } catch (...) {}
    }
    m_ttRef[0] = m_ttRef[1] = nullptr;
    m_nTtItemId = 0;
}

void CUICashShop::ShowItemTip(int itemId, int rx, int ry) {
    if (!m_bTtInit) {
        try {
            reinterpret_cast<void(__thiscall*)(void*)>(kAddr_ToolTip_Ctor)(m_ttBuf);
            m_bTtInit = true;
        } catch (...) {
            LOG_ONCE("cashshopwnd: CUIToolTip ctor threw; item tooltips disabled");
            return;
        }
    }
    // Build the temporary slot only when the item changes: GetItemSlot allocates.
    if (itemId != m_nTtItemId) {
        ReleaseTipItem();
        void* pItemInfo = *reinterpret_cast<void**>(kAddr_CItemInfo_Instance);
        if (pItemInfo) {
            try {
                reinterpret_cast<void*(__thiscall*)(void*, void*, long)>(kAddr_ItemInfo_GetItemSlot)(
                    pItemInfo, m_ttRef, itemId);
            } catch (...) { m_ttRef[0] = m_ttRef[1] = nullptr; }
        }
        m_nTtItemId = itemId;
    }
    try {
        // The CAdminShopDlg call shape: no ItemToolTipParam, no limit-goods, zeros after.
        // A null item (unknown id) makes the client clear the tooltip itself.
        reinterpret_cast<void(__thiscall*)(void*, long, long, void*, void*, void*, int, long, int)>(
            kAddr_ShowItemToolTip)(m_ttBuf, GetAbsLeft() + rx, GetAbsTop() + ry + 0x14,
                                   m_ttRef[1], nullptr, nullptr, 0, 0, 0);
    } catch (...) {
        LOG_ONCE("cashshopwnd: ShowItemToolTip threw for item %d", itemId);
    }
}

void CUICashShop::HideItemTip() {
    if (!m_bTtInit) return;
    try { reinterpret_cast<void(__thiscall*)(void*)>(kAddr_ToolTip_Clear)(m_ttBuf); } catch (...) {}
}

void CUICashShop::OnMouseButton(unsigned int msg, unsigned int /*wParam*/, int rx, int ry) {
    // WM_LBUTTONDBLCLK reaches us VERBATIM. CInputSystem synthesizes it itself at
    // 0x0059AD20 from the user's real GetDoubleClickTime / SM_CXDOUBLECLK / SM_CYDOUBLECLK,
    // both message pumps admit 0x200..0x20A, and CWndMan::ProcessMouse matches none of its
    // special cases for 0x203 so it falls through to OnMouseButton with msg intact
    // (0x009E3FBC `push [ebp+8]`). No GetTickCount detection is needed.
    //
    // The first press of a double is still a real WM_LBUTTONDOWN, so the single-click
    // action has to be a safe prefix of the double-click one -- and it is: click selects
    // and previews, double click pins that same item into the cart.
    if (msg == WM_LBUTTONDBLCLK) {
        // A cart well: take it back out. Symmetric with the gesture that put it in.
        for (int i = 0; i < m_nCartCount; ++i) {
            const int bx = kCartX0 + (i % kCartCols) * (kCartBox + kCartGap);
            const int by = kCartY + 2 + (i / kCartCols) * (kCartBox + kCartGap);
            if (In(rx, ry, bx, by, kCartBox, kCartBox)) {
                CartToggle(m_cart[i].itemId, m_cart[i].price);
                return;
            }
        }
        // A grid cell: pin it.
        std::vector<Entry> vis;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            CollectLocked(vis);
        }
        for (int r = 0; r < kRows; ++r)
            for (int col = 0; col < kCols; ++col)
                if (In(rx, ry, kCellX0 + col * kCellW, kCellY0 + r * kCellH,
                       kCellW - 1, kCellH - 1)) {
                    const int idx = (m_nScroll + r) * kCols + col;
                    if (idx >= 0 && idx < static_cast<int>(vis.size()))
                        CartToggle(vis[idx].itemId, vis[idx].price);
                    return;
                }
        return;
    }

    if (msg == WM_LBUTTONDOWN) {
        if (In(rx, ry, kCloseX, kCloseY, kCloseSize, kCloseSize)) { m_nClosePressed = 1; return; }
        if (In(rx, ry, kBuyX, kBtnY, kBtnW, kBtnH))               { m_nBuyPressed = 1;   return; }
        if (In(rx, ry, kCartBuyX, kBtnY, kBtnW, kBtnH))           { m_nBuyCartPressed = 1; return; }

        // tabs
        for (int i = 0; i < kTabCount; ++i)
            if (In(rx, ry, TabX(i), kTabY, kTabW, kTabH)) { SelectTab(i); return; }

        // the backdrop picker in the preview header
        for (int i = 0; i < kBgCount; ++i)
            if (In(rx, ry, BgTabX(i), kBgTabY, kBgTabW, kBgTabH)) { SelectBackground(i); return; }

        // The search box takes focus when clicked and gives it up on any click outside,
        // which is the only way to get the arrow keys back to the preview.
        if (In(rx, ry, kSearchX, kSearchY, kSearchW, kSearchH)) {
            SearchSetActive(true);
            return;
        }
        SearchSetActive(false);

        // categories: the strip under the red rule. The hit rect is the FULL chip pitch and
        // the band's full height, not the drawn plate -- only the selected chip has a plate,
        // so testing against it would leave every unselected category unclickable.
        {
            const int rows = CatRowCount();
            for (int r = 0; r < rows; ++r)
                if (In(rx, ry, SubTabX(r), kSubY, SubTabW(r), kSubH)) {
                    SelectCat(CatIndexOfRow(r)); return;
                }
        }

        // scrollbar: arrow caps step a row, the bare track pages, the thumb drags
        if (In(rx, ry, kScrollX, kScrollTop, kScrollW, kScrollBot - kScrollTop)) {
            if (ry < kScrollTop + kSbArrowH)  { --m_nScroll; ClampScroll(); return; }
            if (ry >= kScrollBot - kSbArrowH) { ++m_nScroll; ClampScroll(); return; }
            RECT th;
            ThumbRect(th, VisibleCount());
            if (ry >= th.top && ry < th.bottom) {
                m_bSbDrag   = 1;
                m_nSbGrabDY = ry - th.top;
            } else {
                m_nScroll += (ry < th.top) ? -kRows : kRows;
                ClampScroll();
            }
            return;
        }

        // cells
        std::vector<Entry> vis;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            CollectLocked(vis);
        }
        for (int r = 0; r < kRows; ++r)
            for (int col = 0; col < kCols; ++col)
                if (In(rx, ry, kCellX0 + col * kCellW, kCellY0 + r * kCellH, kCellW - 1, kCellH - 1)) {
                    const int idx = (m_nScroll + r) * kCols + col;
                    if (idx >= 0 && idx < static_cast<int>(vis.size()))
                        SelectItem(vis[idx].itemId);
                    return;
                }

        // Title bar -> drag. Tested LAST, after every other hit test, so it can never
        // steal a click from a control that happens to overlap the caption row.
        if (ry < kTitleY + kTitleH) {
            POINT sp;
            if (GetAbsCursor(sp)) {
                HideItemTip();
                m_bDragging = 1;
                m_nDragAnchorX = sp.x - m_screenX;
                m_nDragAnchorY = sp.y - m_screenY;
            }
        }
        return;
    }

    if (msg == WM_LBUTTONUP) {
        if (m_nClosePressed) {
            m_nClosePressed = 0;
            if (In(rx, ry, kCloseX, kCloseY, kCloseSize, kCloseSize)) { Destroy(); return; }
        }
        if (m_nBuyPressed) {
            m_nBuyPressed = 0;
            if (m_nSelItemId && In(rx, ry, kBuyX, kBtnY, kBtnW, kBtnH)) {
                play_ui_sound(L"BtMouseClick");
                _snprintf(g_szStatus, sizeof(g_szStatus), "Buying...");
                g_szStatus[sizeof(g_szStatus) - 1] = 0;
                SendBuy(m_nSelItemId);
            }
        }
        if (m_nBuyCartPressed) {
            m_nBuyCartPressed = 0;
            if (m_nCartCount > 0 && In(rx, ry, kCartBuyX, kBtnY, kBtnW, kBtnH)) {
                play_ui_sound(L"BtMouseClick");
                m_nSentCount = m_nCartCount;
                for (int i = 0; i < m_nSentCount; ++i) m_sentIds[i] = m_cart[i].itemId;
                _snprintf(g_szStatus, sizeof(g_szStatus),
                          "Buying %d item%s...", m_nSentCount, m_nSentCount == 1 ? "" : "s");
                g_szStatus[sizeof(g_szStatus) - 1] = 0;
                SendBuyCart(m_sentIds, m_nSentCount);
            }
        }
        m_bDragging = 0;
        m_bSbDrag   = 0;
        InvalidateRect(nullptr);
    }
}

// =====================================================
// OPEN
// =====================================================
// Keep the whole window on screen. This is NOT cosmetic: the position is remembered
// across opens, and when the window grew from 600x376 to 760x540 a remembered
// top-left that used to be fine (the old centred 100,112) put the right and bottom
// edges past an 800x600 screen. The window still existed and still drew -- a stack in
// the client log proved it was painting -- it was simply off screen, which looks exactly
// like "the Cash button does nothing".
void ClampToScreen(int& x, int& y) {
    const int sw = get_screen_width();
    const int sh = get_screen_height();
    if (x > sw - kWndW) x = sw - kWndW;
    if (y > sh - kWndH) y = sh - kWndH;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
}

void OpenWindow() {
    if (CUICashShop::ms_pInstance) {
        CUICashShop::ms_pInstance->Destroy();
        return;                       // second press closes it, like the bag button
    }
    int x = s_bSavedPos ? s_savedX : (get_screen_width()  - kWndW) / 2;
    int y = s_bSavedPos ? s_savedY : (get_screen_height() - kWndH) / 2;
    ClampToScreen(x, y);
    LogMessage("Cash Shop window: opening at %d,%d (screen %dx%d)",
               x, y, get_screen_width(), get_screen_height());
    new CUICashShop(x, y);
}

} // namespace CashShopWnd

// =====================================================
// ROUTED FROM PacketDispatcher.cpp  (RECEIVE THREAD)
// =====================================================
void CashShopWnd_HandleSync(CInPacket* pPacket) {
    using namespace CashShopWnd;
    if (!pPacket) return;

    Reader r(pPacket);
    r.Skip2();                                   // the opcode itself, relative to entry
    const unsigned char resp = r.Decode1();

    switch (resp) {
        case kResp_Open: {
            const long long a = r.Decode8();   // NX Credit: writeLong since the NX uncap
            const int b = r.Decode4(), c = r.Decode4();
            if (r.bad) return;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_cash[0] = a; g_cash[1] = b; g_cash[2] = c;
            }
            g_bWantOpen.store(true);             // the window is created on the main thread
            break;
        }
        case kResp_Cash: {
            const long long a = r.Decode8();   // NX Credit: writeLong since the NX uncap
            const int b = r.Decode4(), c = r.Decode4();
            if (r.bad) return;
            std::lock_guard<std::mutex> lk(g_mtx);
            g_cash[0] = a; g_cash[1] = b; g_cash[2] = c;
            g_bCatalogDirty.store(true);
            break;
        }
        case kResp_Catalog: {
            const unsigned char flag = r.Decode1();      // 1 = clear this category, 0 = append
            const int tab = r.Decode1();
            const int cat = r.Decode1();
            const int count = static_cast<unsigned short>(r.Decode2());
            if (r.bad || count < 0 || count > kMaxEntriesPerChunk) return;

            // Parse into a local vector and swap only on success: a truncated chunk
            // must be rejected whole rather than half-applied.
            std::vector<Entry> parsed;
            parsed.reserve(count);
            for (int i = 0; i < count; ++i) {
                Entry e;
                e.itemId = r.Decode4();
                e.price  = r.Decode4();
                e.count  = static_cast<unsigned short>(r.Decode2());
                e.tab    = r.Decode1();
                e.cat    = r.Decode1();
                e.name   = r.DecodeStr();
                if (r.bad) return;                       // whole chunk dropped
                parsed.push_back(std::move(e));
            }
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                const int key = BucketKey(tab, cat);
                std::vector<Entry>& bucket = g_buckets[key];
                if (flag == 1) bucket.clear();
                // The guard is per CATEGORY now. Crossing it drops the chunk rather than
                // the shop, and the category is still marked loaded so the window stops
                // waiting on it.
                if (bucket.size() + parsed.size() <= static_cast<size_t>(kMaxPerCategory))
                    for (Entry& e : parsed) bucket.push_back(std::move(e));
                g_pending.erase(key);
                g_loaded.insert(key);
            }
            g_bCatalogDirty.store(true);
            break;
        }
        case kResp_Index: {
            const int n = static_cast<unsigned short>(r.Decode2());
            if (r.bad || n < 0 || n > 4096) return;
            std::map<int, int> counts;
            for (int i = 0; i < n; ++i) {
                const int tab = r.Decode1();
                const int cat = r.Decode1();
                const int cnt = r.Decode4();
                if (r.bad) return;                       // whole index dropped
                if (cnt >= 0) counts[BucketKey(tab, cat)] = cnt;
            }
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_counts.swap(counts);
                // A fresh index means the shop was rebuilt, so anything already fetched is
                // now suspect; drop it and let the categories be re-requested on demand.
                g_buckets.clear();
                g_loaded.clear();
                g_pending.clear();
            }
            g_bCatalogDirty.store(true);
            break;
        }
        case kResp_Buy: {
            const unsigned char code = r.Decode1();
            const int itemId = r.Decode4();
            if (r.bad) return;
            const char* m = (code < _countof(kBuyMsg)) ? kBuyMsg[code] : "Purchase failed.";
            _snprintf(g_szStatus, sizeof(g_szStatus), "%s (item %d)", m, itemId);
            g_szStatus[sizeof(g_szStatus) - 1] = 0;
            g_bCatalogDirty.store(true);
            break;
        }
        case kResp_BuyCart: {
            const unsigned char code = r.Decode1();
            const int itemId = r.Decode4();              // the item that FAILED, 0 on success
            const unsigned char delivered = r.Decode1();
            const int spent = r.Decode4();
            if (r.bad) return;
            if (code == 0) {
                _snprintf(g_szStatus, sizeof(g_szStatus), "Bought %d item%s for %d NX.",
                          delivered, (delivered == 1) ? "" : "s", spent);
                g_bCartBought.store(true);
            } else {
                const char* m = (code < _countof(kBuyMsg)) ? kBuyMsg[code] : "Purchase failed.";
                // The cart is left intact for the player to fix rather than silently
                // emptied. `delivered` is normally 0 -- the server validates the whole cart
                // before it touches anything -- but it is reported rather than asserted
                // away, because the one path that can deliver-then-fail charges nothing,
                // and a player who is told "nothing was bought" while holding a free item
                // has no way to notice.
                if (delivered) {
                    _snprintf(g_szStatus, sizeof(g_szStatus),
                              "%s (item %d) %d item(s) arrived free; you were not charged.",
                              m, itemId, delivered);
                } else if (itemId) {
                    _snprintf(g_szStatus, sizeof(g_szStatus), "%s (item %d) Nothing was bought.", m, itemId);
                } else {
                    _snprintf(g_szStatus, sizeof(g_szStatus), "%s Nothing was bought.", m);
                }
            }
            g_szStatus[sizeof(g_szStatus) - 1] = 0;
            g_bCatalogDirty.store(true);
            break;
        }
        default:
            break;
    }
}

// =====================================================
// MAIN-THREAD DRIVER  (bypass.cpp CWvsApp::CallUpdate_hook)
// =====================================================
// CWvsContext::TryCloseUI — v95 sym, v83 VA 0x00A06E55 (ret 4). Stock returns 0 for any
// window it does not know, which makes Esc open the game menu instead of closing us.
// The storage bag (storagebag.cpp) shares this one detour.
static auto CWvsContext__TryCloseUI =
    reinterpret_cast<int(__thiscall*)(void*, void*)>(CashShopWnd::kAddr_TryCloseUI);

static int __fastcall CWvsContext__TryCloseUI_hook(void* pThis, void* _EDX, void* pWnd) {
    CashShopWnd::CUICashShop* pShop = CashShopWnd::CUICashShop::ms_pInstance;
    if (pWnd && pShop && pWnd == static_cast<CWnd*>(pShop)) {
        pShop->Destroy();                  // OnDestroy plays MenuDown and leaves the stack
        return 1;
    }
    if (StorageBag_TryCloseUI(pWnd)) {
        return 1;
    }
    return CWvsContext__TryCloseUI(pThis, pWnd);
}

void AttachCashShopWndMod() {
    ATTACH_HOOK(CWvsContext__TryCloseUI, CWvsContext__TryCloseUI_hook);
}

// Live NX from SendOpcode.INVENTORY_CASH (inventorynx.cpp), which the server sends after every NX
// Credit change -- so a gain while the window is open shows at once rather than on the next open.
void CashShopWnd_SetNxCredit(long long nNxCredit) {
    using namespace CashShopWnd;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_cash[0] = nNxCredit;
    }
    g_bCashDirty.store(true);
}

void CashShopWnd_Tick() {
    using namespace CashShopWnd;
    if (g_bWantOpen.exchange(false)) {
        g_szStatus[0] = 0;
        OpenWindow();
    }
}

