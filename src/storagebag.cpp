// ============================================================
// storagebag.cpp  —  the STORAGE BAG window: Ore / Scroll / Chair / Cash bags in one window
//
// Ported from Kaentake/kaentake-main/implemented/storagebag. One CWnd subclass
// (CUIBagWindow), a single instance, shows one bag kind at a time, picked by the
// ORE/SCROLL/CHAIR/CASH tabs. The body is a 5x5 scrollable item grid with native tooltips
// and native drag: drag OUT -> withdraw to inventory, drag an item IN from the inventory ->
// deposit into the active bag. The server is authoritative: every action is a request, and
// the reply is a full snapshot of that bag.
//
// Opened by the Key Config shortcut type 4 id 56 (damagerankinput.cpp, default F9) or by the
// BAG button this file adds to the item inventory window.
//
// Art: the custom chrome ships in Custom.wz at UI/UIWindow.img/Bag and is read through the
// Custom/ prefix, because resman.cpp's merge does not surface a new sub-property under an
// existing .img. The "Satchel" theme (leather frame, parchment cells) owns the background, the
// tab pills and labels, the scrollbar and the AUTO art, all under Bag/. Stock pieces (Basic.img
// ItemNo digits, UIWindow.img/Item/BtSort) are read from UI/ as usual. The original used UIWindow.img/Android/BtSort, which v83 does not have.
//
// Every address below was checked against v83.idb and this MapleStory.exe (image base
// 0x00400000). The v95 PDB names are noted where the IDB leaves a function unnamed.
// ============================================================

#include "pch.h"
#include "hook.h"
#include "debug.h"
#include "storagebag.h"
#include "clientsocket.h"
#include "wvs/packet.h"
#include "wvs/field.h"
#include "wvs/wnd.h"
#include "wvs/iteminfo.h"
#include "wvs/util.h"
#include "wvs/wvsapp.h"
#include "wvs/wndman.h"
#include "wvs/ctrlwnd.h"     // CCtrlButton (+ nested CREATEPARAM) for the inventory-window button
#include "ztl/ztl.h"         // ZRef / ZRefCounted / ZAllocEx

#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <cctype>

namespace BagWindow {

// ---------------------------------------------------------------------------
// Item-name lookup (for the search bar) — reads the display name out of String.wz
// via the resource manager (same get_rm() pattern getwzinfo.cpp uses), lowercased
// + cached so a keystroke filter never re-hits the WZ for the same item.
//
// Names live in per-category top-level images: Consume.img (scrolls/Use), Etc.img
// (ores), Ins.img, Cash.img. Consume/Ins/Cash are FLAT (<id>/name); Etc.img nests
// its entries under an "Etc" wrapper (Etc.img/Etc/<id>/name) — so we try the flat
// path first, then the wrapper.
// ---------------------------------------------------------------------------
static const wchar_t* ItemCatImg(int id) {
    switch (id / 1000000) {
        case 2:  return L"Consume";   // Use — scrolls (204xxxx) + White Scroll (2340000)
        case 3:  return L"Ins";
        case 4:  return L"Etc";       // ores / maker materials (wrapped under "Etc")
        case 5:  return L"Cash";
        default: return nullptr;
    }
}
static const std::string& GetItemNameLower(int id) {
    static std::unordered_map<int, std::string> s_cache;
    auto it = s_cache.find(id);
    if (it != s_cache.end()) return it->second;
    std::string name;
    const wchar_t* img = ItemCatImg(id);
    if (img) {
        try {
            std::wstring path = std::wstring(L"String/") + img + L".img";
            IWzPropertyPtr root = get_rm()->GetObjectA(path.c_str()).GetUnknown();
            if (root) {
                Ztl_bstr_t sId = std::to_wstring(id).c_str();
                IWzPropertyPtr node = root->item[sId].GetUnknown();        // flat (Consume/Ins/Cash)
                if (!node) {
                    IWzPropertyPtr wrap = root->item[img].GetUnknown();    // wrapper (Etc.img/Etc)
                    if (wrap) node = wrap->item[sId].GetUnknown();
                }
                if (node) {
                    Ztl_variant_t v = node->item[L"name"];
                    if (v.vt == VT_BSTR) name = (const char*)_bstr_t(v);
                }
            }
        } catch (...) {}
    }
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    return s_cache.emplace(id, std::move(name)).first->second;
}

// ---------------------------------------------------------------------------
// Addresses & thin wrappers
// ---------------------------------------------------------------------------
static constexpr uintptr_t kAddr_CWvsContext_Instance  = 0x00BE7918; // TSingleton<CWvsContext>::ms_pInstance
static constexpr uintptr_t kAddr_TSecType_long_GetData = 0x0042873D; // TSecType<long>::GetData, v83 VA 0x0042873D (item+0xC plaintext itemID)
static constexpr uintptr_t kAddr_play_ui_sound         = 0x00989588; // play_ui_sound — v95 sym, v83 VA 0x00989588 (cdecl)
static constexpr uintptr_t kAddr_get_basic_font        = 0x0098A707; // get_basic_font — v95 sym, v83 VA 0x0098A707 (cdecl)
static constexpr uintptr_t kAddr_ProcessBasicUIKey     = 0x00A07431; // CWvsContext::ProcessBasicUIKey — v95 sym, v83 VA 0x00A07431 (ret 8)
static constexpr uintptr_t kAddr_SetFont               = 0x0046341A; // IWzFont::Create wrapper, v83 VA 0x0046341A (ret 0x10; throws on failure)
static constexpr uintptr_t kAddr_TT_Ctor               = 0x008E49B5; // CUIToolTip::CUIToolTip — v95 sym, v83 VA 0x008E49B5
static constexpr uintptr_t kAddr_TT_Dtor               = 0x008E6BA3; // CUIToolTip::~CUIToolTip — v95 sym, v83 VA 0x008E6BA3
static constexpr uintptr_t kAddr_TT_Clear              = 0x008E6E23; // CUIToolTip::ClearToolTip — v95 sym, v83 VA 0x008E6E23
static constexpr uintptr_t kAddr_ShowItemToolTip       = 0x008F5B20; // CUIToolTip::ShowItemToolTip — v95 sym, v83 VA 0x008F5B20 (ret 0x20)

static auto play_ui_sound  = reinterpret_cast<void(__cdecl*)(const wchar_t*)>(kAddr_play_ui_sound);
static auto get_basic_font = reinterpret_cast<IWzFontPtr*(__cdecl*)(IWzFontPtr*, int)>(kAddr_get_basic_font);

typedef void(__thiscall* t_TT)(void*);
static auto TT_Ctor  = reinterpret_cast<t_TT>(kAddr_TT_Ctor);
static auto TT_Dtor  = reinterpret_cast<t_TT>(kAddr_TT_Dtor);
static auto TT_Clear = reinterpret_cast<t_TT>(kAddr_TT_Clear);
typedef void(__thiscall* t_ShowItemToolTip)(void*, int, int, void*, void*, void*, int, int, int);
static auto ShowItemToolTip = reinterpret_cast<t_ShowItemToolTip>(kAddr_ShowItemToolTip);

typedef int(__thiscall* t_GetData)(const void*);
static auto TSecType_long_GetData = reinterpret_cast<t_GetData>(kAddr_TSecType_long_GetData);

static void* GetWvsContext() { return *reinterpret_cast<void**>(kAddr_CWvsContext_Instance); }

// Plaintext itemID from a GW_ItemSlotBase* (TSecType<long> at +0xC).
static int DecodeItemID(void* pItem) {
    if (!pItem) return 0;
    int id = 0;
    __try { id = TSecType_long_GetData(reinterpret_cast<const char*>(pItem) + 0x0C); }
    __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
    return id;
}

// ---------------------------------------------------------------------------
// Native drag SOURCE.
// ---------------------------------------------------------------------------
static constexpr int kDragIconW = 44, kDragIconH = 44, kDragIconZ = 9999;
// CWndMan::BeginDragDrop — v83 VA 0x009E353D. The v95 symbol takes a third int, but this build is
// `ret 8`: two args, the same (handler, draggable) pair CUIItem passes at 0x0081D83D.
typedef void(__thiscall* t_BeginDragDrop)(void* wndMan, void* src, void* drag);
static auto BeginDragDrop = reinterpret_cast<t_BeginDragDrop>(0x009E353D);
// CWndMan::EndDragDrop(rx, ry, bDblClk) — v83 VA 0x009E37C2. The engine's own "finish the active drag"
// entry (fires the draggable's OnDropped, or OnDoubleClicked when bDblClk!=0). __thiscall,
// retn 0xC. We call it from the per-frame release poll (PollDragRelease) so a natural
// press-drag-RELEASE completes the drop: this client only ends a drag on the NEXT click,
// never on button-up (verified in CWndMan::ProcessMouse — WM_LBUTTONUP is not an end trigger).
typedef void(__thiscall* t_EndDragDrop)(void* wndMan, int rx, int ry, int bDblClk);
static auto EndDragDrop = reinterpret_cast<t_EndDragDrop>(0x009E37C2);
// Draggable base ctor, v83 VA 0x006FFDA3 (ret 4; unnamed in the IDB). It sets the vtable 0x00AF34E8
// and AddRefs the icon layer into +0xC. The stock inventory drag (CUIItem, 0x0081D7FD) calls it on a
// 0x28-byte ZAlloc block, fills +0x18 type / +0x1C slot / +0x20 0 / +0x24 handler, then swaps in the
// CDraggableItem vtable 0x00AF34D8 ([1] OnDropped 0x004EF140, [2] OnDoubleClicked 0x004EFD25).
typedef void*(__thiscall* t_IDraggable_Init)(void* self, void* icon);
static auto IDraggable_Init = reinterpret_cast<t_IDraggable_Init>(0x006FFDA3);
static constexpr uintptr_t kCDraggableItem_Vtable = 0x00AF34D8;
// The CDraggableItem MUST come from the GAME's ZAlloc so the engine drag manager recognizes it.
// A C++ reimplementation of the allocator produces an object the drag manager rejects
// (drag never starts -> no ghost icon). Call the real allocator: Alloc(this=instance, size).
typedef void*(__thiscall* t_ZAlloc_Alloc)(void* self, size_t size);
static auto ZAlloc_Alloc = reinterpret_cast<t_ZAlloc_Alloc>(0x00403065); // ZAllocEx<ZAllocAnonSelector>::Alloc (ret 4)
static constexpr uintptr_t kZAlloc_Instance = 0x00BF0B00;                 // ZAllocEx<ZAllocAnonSelector>::_s_alloc

// pOrigin: HUD origin borrowed from a REAL CWnd (the bag window's own layer). We do
// NOT call CWndMan::GetOrgWindowEx() here — it is dead in this client (ResetOrgWindow
// is never called) and THREW, which aborted this whole function to its catch and
// returned null, so BeginDragDrop never ran and no drag ever started. Borrowing the
// window's live origin (and letting BeginDragDrop overwrite it anyway) fixes that.
static IWzGr2DLayer* CreateDragIconLayer(int itemID, int hudX, int hudY, IUnknown* pOrigin) {
    if (!itemID) return nullptr;
    try {
        IWzGr2DPtr& gr = get_gr();
        if (!gr) return nullptr;
        CItemInfo* pItemInfo = CItemInfo::GetInstance();
        if (!pItemInfo) return nullptr;
        IWzCanvasPtr canvas;
        PcCreateObject<IWzCanvasPtr>(L"Canvas", canvas, nullptr);
        if (!canvas) return nullptr;
        canvas->Create(kDragIconW, kDragIconH, vtMissing, vtMissing);
        pItemInfo->DrawItemIconForSlot(canvas, itemID, 6, kDragIconH - 6, 0, 0, 0, 0, 0, 0);
        IWzGr2DLayerPtr layer = gr->CreateLayer(0, 0, kDragIconW, kDragIconH, kDragIconZ,
                                                static_cast<IUnknown*>(canvas), vtMissing);
        if (!layer) return nullptr;
        if (pOrigin) { try { layer->origin = pOrigin; } catch (...) {} }
        layer->width = kDragIconW; layer->height = kDragIconH; layer->color = 0xFFFFFFFF;
        layer->RelMove(hudX - kDragIconW / 2, hudY - kDragIconH / 2);
        layer->visible = 1;
        IWzGr2DLayer* raw = layer; raw->AddRef();
        return raw;
    } catch (...) { return nullptr; }
}

static IWzGr2DLayer* s_pDragIcon = nullptr;
// True while a native item-drag started from the bag is in flight. The engine owns
// mouse capture during a drag and can re-dispatch (clamped) input to the window;
// this flag lets us suppress scroll changes so picking an item up can't scroll
// the grid. Cleared on drop (OnDropped hook), on button-up, and by EndDragIcon.
static bool s_bItemDragging = false;
static void EndDragIcon() {
    s_bItemDragging = false;
    if (!s_pDragIcon) return;
    try { s_pDragIcon->visible = 0; } catch (...) {}
    s_pDragIcon->Release();
    s_pDragIcon = nullptr;
}

// True while the engine has ANY drag-drop in flight, no matter where it started —
// a bag item being dragged out, OR a player-inventory (or other window) item being
// dragged IN/over the bag. CWndMan::BeginDragDrop stores the drag source handler at
// CWndMan+0x90 and ClearDragContext zeroes it on drop, so a non-null value there is
// the global "a drag is happening" signal. (Grabbing our own scrollbar thumb does
// NOT call BeginDragDrop, so this stays false then — legitimate scrolling is fine.)
static bool IsEngineDragActive() {
    if (!CWndMan::IsInstantiated()) return false;
    CWndMan* wm = CWndMan::GetInstance();
    if (!wm) return false;
    void* src = nullptr;
    __try { src = *reinterpret_cast<void**>(reinterpret_cast<char*>(wm) + 0x90); }
    __except (EXCEPTION_EXECUTE_HANDLER) { src = nullptr; }
    return src != nullptr;
}
// The scrollbar (wheel, arrows, thumb grab) is locked while any item drag is in
// flight: our own bag drag-out (s_bItemDragging) or an engine drag in/over the
// window (IsEngineDragActive). Only deliberate scrolling — wheel or a thumb grab
// with no drag active — moves the grid.
static bool ScrollLocked() { return s_bItemDragging || IsEngineDragActive(); }

// ---------------------------------------------------------------------------
// Esc: the client's close stack (CWvsContext::m_apStackForTab), as in cashshopwnd.cpp. Esc in
// CWvsContext::ProcessBasicUIKey (0x00A074A5) hands GetTopStackForTab() to TryCloseUI; the bag
// joins the stack when created, leaves it in OnDestroy, and is closed by cashshopwnd.cpp's
// TryCloseUI hook through StorageBag_TryCloseUI (one detour on that address, not two).
// ---------------------------------------------------------------------------
static constexpr uintptr_t kAddr_PushStackForTab       = 0x0091BE95; // CWvsContext::PushStackForTab — v95 sym, v83 VA 0x0091BE95 (ret 4)
static constexpr uintptr_t kAddr_RemoveFromStackForTab = 0x007FE499; // CWvsContext::RemoveFromStackForTab — v95 sym, v83 VA 0x007FE499 (ret 4; absent entry is a no-op)
static constexpr uintptr_t kAddr_GetTopStackForTab     = 0x00A07599; // CWvsContext::GetTopStackForTab — v95 sym, v83 VA 0x00A07599 (ret 0)

static void StackRemove(CWnd* pWnd) {
    void* ctx = GetWvsContext();
    if (!ctx) return;
    __try {
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kAddr_RemoveFromStackForTab)(ctx, pWnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Moves the window to the top of the stack, so it is the next thing Esc closes.
static void StackRaise(CWnd* pWnd) {
    void* ctx = GetWvsContext();
    if (!ctx) return;
    __try {
        void* top = reinterpret_cast<void*(__thiscall*)(void*)>(kAddr_GetTopStackForTab)(ctx);
        if (top == pWnd) return;
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kAddr_RemoveFromStackForTab)(ctx, pWnd);
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kAddr_PushStackForTab)(ctx, pWnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------
// GW_ItemSlotBase decode (for native tooltips).
// ---------------------------------------------------------------------------
struct GW_ItemSlotBase : public ZRefCounted {
    virtual ~GW_ItemSlotBase() = 0;
    virtual int IsProtectedItem() = 0;
    virtual int IsPreventSlipItem() = 0;
    virtual int IsSupportWarmItem() = 0;
    virtual int IsBindedItem() = 0;
    virtual int IsPossibleTradingItem() = 0;
    virtual int GetType() = 0;
};
struct ZRefOut { void* unused; void* item; };
// GW_ItemSlotBase::Decode — v83 VA 0x004E33F9 (cdecl, hidden ZRef return). The engine ZRef is
// { base, p@+4 }; Decode stores p at +4 already AddRef'd, so the caller owns one reference.
static auto GW_ItemSlotBase_Decode =
    reinterpret_cast<int(__cdecl*)(void* /*ZRefOut*/, CInPacket*)>(0x004E33F9);

static GW_ItemSlotBase* RawDecodeItem(CInPacket* pkt) {
    ZRefOut out = {};
    __try { GW_ItemSlotBase_Decode(&out, pkt); }
    __except (EXCEPTION_EXECUTE_HANDLER) { out.item = nullptr; }
    return reinterpret_cast<GW_ItemSlotBase*>(out.item);
}
static void StoreDecoded(ZRef<GW_ItemSlotBase>& dst, GW_ItemSlotBase* p) {
    if (p) dst = ZRef<GW_ItemSlotBase>(p, false);
    else   dst = ZRef<GW_ItemSlotBase>();
}
static void DropRef(GW_ItemSlotBase* p) { if (p) { ZRef<GW_ItemSlotBase> t(p, false); } }

// ---------------------------------------------------------------------------
// Protocol (must match RecvOpcode/SendOpcode.BAG_WINDOW = 0x3724/0x3725).
// Opcodes live in storagebag.h so clientsocket.cpp can route the reply.
// ---------------------------------------------------------------------------
static constexpr int kReq_Open = 0, kReq_Withdraw = 1, kReq_Deposit = 2, kReq_Merge = 3, kReq_Close = 4, kReq_SetAuto = 5;
static constexpr int kResp_Snapshot = 1;   // reply to a request: show it, opening the window if closed
static constexpr int kResp_Refresh = 2;    // server push (pickup auto-collect): update the cache only
static constexpr int kKindOre = 0, kKindScroll = 1, kKindChair = 2, kKindCash = 3;
static constexpr int kKindCount = 4;

// ---------------------------------------------------------------------------
// Geometry — matches the storage-bag art (UI/UIWindow.img/Bag/backgrnd, 207x248):
// a shared background with a 5x5 grid. The four bag kinds are selected by clickable
// tab labels (ORE/SCROLL/CHAIR/CASH) in the gray strip under the title, vanilla-style.
// ---------------------------------------------------------------------------
static constexpr int kInvCols     = 5;
static constexpr int kInvVisRows  = 5;
static constexpr int kInvVisCount = kInvCols * kInvVisRows;   // 25
static constexpr int kInvCap      = 200;                      // bag slot capacity (matches server)
static constexpr int kInvTotRows  = kInvCap / kInvCols;       // 40

static constexpr int kWndW = 207;
static constexpr int kWndH = 248;
static constexpr int kTitleH = 20;            // title bar (drag region); tabs sit below it at y~22..40

// Grid (cell wells measured off the art: col0 x=8, 36px pitch; row0 y=50, 34px pitch; cell ~31px).
static constexpr int kCell     = 31;
static constexpr int kColW     = 36;
static constexpr int kRowH     = 34;
static constexpr int kGridLeft = 8;
static constexpr int kGridTop  = 50;
static constexpr int kIconDX   = 0;
static constexpr int kIconBY   = kCell - 1;

// Scrollbar — Satchel Bag/scroll/{prev,next,base,thumb}, overlaid in the right margin (col5 ends at x=183).
static constexpr int kScrollW   = 15;
static constexpr int kScrollX   = 186;
static constexpr int kSbArrowH  = 13;
static constexpr int kScrollTop = kGridTop;                                  // 50
static constexpr int kScrollBot = kGridTop + (kInvVisRows - 1) * kRowH + kCell; // 217
static constexpr int kThumbMinH = 20;

// Search field — baked recessed white box near the bottom; we draw text/caret/hint/count on top.
static constexpr int kSearchBoxL   = 8;
static constexpr int kSearchBoxR   = 199;
static constexpr int kSearchBoxTop = 222;
static constexpr int kSearchBoxBot = 242;
static constexpr int kSearchTextX  = 13;   // 5px inside the box border (x=8)
static constexpr int kSearchTextY  = 227;
static constexpr int kSearchMax    = 32;
static constexpr unsigned long kSatchelInk = 0xFF5C3414;   // search text / caret: the title plate's brown

// Title-bar buttons overlaid on the RIGHT (the art bakes no button wells): sort/merge + close.
static constexpr int kBtMergeX  = 173, kBtMergeY  = 5, kBtMergeW  = 12, kBtMergeH  = 12;
// AUTO (pickup auto-collect for the active tab, = @orebag/@scrollbag/@chairbag/@cashbag on|off), left of sort.
static constexpr int kBtAutoW   = 31,  kBtAutoH   = 12, kBtAutoX = kBtMergeX - 2 - kBtAutoW, kBtAutoY = 5;
static constexpr int kBtCloseX  = 187, kBtCloseY  = 5, kBtCloseW  = 12, kBtCloseH  = 12;

// Tab bar — four clickable tab labels in the gray strip below the title. Each tab's hit-rect is
// one quarter of the label row; the label art (17/33/25/23 px wide) is centered within its slot.
static constexpr int kTabTop = 22, kTabBot = 40;
static constexpr int kTabHitLeft[kKindCount]  = {   8,  56, 104, 152 };  // per-tab slot left
static constexpr int kTabHitRight[kKindCount] = {  56, 104, 152, 200 };  // per-tab slot right
// Satchel tab pill (Bag/tab/{on,off}/{left,fill,right}, 3-slice) drawn behind each label: parchment when
// active, dark leather otherwise (the "off" art is transparent in its top 2 rows, so it sits lower).
// grey otherwise — matching the item inventory tabs. Bottom meets the red separator line.
static constexpr int kPillTop = 21;    // pill top y
static constexpr int kPillH   = 19;    // Bag/tab fill height
static constexpr int kPillPad = 2;     // inset of the pill within its tab slot
static constexpr int kTabLabelY = kPillTop + (kPillH - 5) / 2;   // 28: center the 5px label on the pill

// Decoded bag contents, shared cache (the window is a thin view over the active one).
struct BagStore {
    ZRef<GW_ItemSlotBase> obj[kInvCap];
    int  id[kInvCap];
    int  qty[kInvCap];     // stack count per slot (from the snapshot's trailing block; 0 if unknown)
    int  count;
    bool ready;
};
static BagStore g_bag[kKindCount];   // indexed by kind (ore/scroll/chair/cash)

// True from the moment a transfer request (withdraw/deposit) is sent until the
// next snapshot lands. The server bag is COMPACTED (takeOut removes by identity,
// shifting later slots down), so the dense slot indices the client learned from
// the last snapshot go stale the instant a transfer is in flight. While awaiting
// the reply we freeze grid interaction so a second rapid gesture can't operate on
// a now-shifted index and withdraw the wrong item. Cleared in HandleBagSnapshot.
static bool s_bAwaitingSnapshot = false;

// Per-tab pickup auto-collect state, as last reported by the server (the snapshot's trailing byte).
static bool s_bAutoOn[kKindCount] = {};

// Set while the window is torn down for a stage change: the socket may already be heading for
// the login server, so OnDestroy must not send CLOSE (the new channel starts with no active bag).
static bool s_bLeavingField = false;

// Remembered window placement, so reopening lands where it was last closed and on
// the bag last viewed.
static bool s_bSavedPos = false;
static int  s_savedX = 0, s_savedY = 0;
static int  s_savedKind = kKindOre;

static void FreeBag(int kind) {
    if (kind < 0 || kind >= kKindCount) return;
    BagStore& s = g_bag[kind];
    for (int i = 0; i < kInvCap; ++i) { s.obj[i] = ZRef<GW_ItemSlotBase>(); s.id[i] = 0; s.qty[i] = 0; }
    s.count = 0;
    s.ready = false;
}

// --- send ---
static void SendBagPacket(const COutPacket& o) { SendClientPacket(o); }
static void SendBagReq_Open(int kind) {
    COutPacket o(kStorageBagRequestOpcode);
    o.Encode1((unsigned char)kReq_Open); o.Encode1((unsigned char)kind);
    SendBagPacket(o);
}
static void SendBagReq_Withdraw(int kind, int srcSlot) {
    COutPacket o(kStorageBagRequestOpcode);
    o.Encode1((unsigned char)kReq_Withdraw); o.Encode1((unsigned char)kind);
    o.Encode2((unsigned short)srcSlot);
    SendBagPacket(o);
    s_bAwaitingSnapshot = true;   // slot indices go stale until the reply snapshot
}
static void SendBagReq_Deposit(int kind, int invType, int invPos) {
    COutPacket o(kStorageBagRequestOpcode);
    o.Encode1((unsigned char)kReq_Deposit); o.Encode1((unsigned char)kind);
    o.Encode2((unsigned short)invType); o.Encode2((unsigned short)invPos);
    SendBagPacket(o);
    s_bAwaitingSnapshot = true;   // slot indices go stale until the reply snapshot
}
// The window closed: the server stops pushing auto-collect refreshes. No reply.
static void SendBagReq_Close() {
    COutPacket o(kStorageBagRequestOpcode);
    o.Encode1((unsigned char)kReq_Close); o.Encode1(0);
    SendBagPacket(o);
}
// Turn this tab's pickup auto-collect on/off; the reply snapshot carries the new state.
static void SendBagReq_SetAuto(int kind, bool on) {
    COutPacket o(kStorageBagRequestOpcode);
    o.Encode1((unsigned char)kReq_SetAuto); o.Encode1((unsigned char)kind);
    o.Encode1(on ? 1 : 0);
    SendBagPacket(o);
}
// Ask the server to merge identical stacks + compact the active bag; replies snapshot.
static void SendBagReq_Merge(int kind) {
    COutPacket o(kStorageBagRequestOpcode);
    o.Encode1((unsigned char)kReq_Merge); o.Encode1((unsigned char)kind);
    SendBagPacket(o);
    s_bAwaitingSnapshot = true;   // slot indices go stale until the reply snapshot
}

// ---------------------------------------------------------------------------
// CUIBagWindow — single window showing one bag (Ore or Scroll), flipped by the
// title-bar SWITCH button.
// ---------------------------------------------------------------------------
class CUIBagWindow : public CWnd {
public:
    ZALLOC_GLOBAL
    inline static CUIBagWindow* ms_pInstance = nullptr;
    inline static CRTTI ms_RTTI{ nullptr };

    int  m_activeKind;            // currently shown bag kind: 0=ore 1=scroll 2=chair 3=cash
    int  m_nInvScroll;
    int  m_screenX, m_screenY;

    RECT m_rcClose;
    RECT m_rcTab[kKindCount];    // clickable tab labels (select the bag kind)
    RECT m_rcMerge;              // sort/merge button (consolidate stacks)
    RECT m_rcAuto;               // AUTO button (pickup auto-collect for the active tab)
    int  m_nAutoHover, m_nAutoPressed;
    int  m_nCloseHover;
    int  m_nMergeHover, m_nMergePressed;
    int  m_bDragging, m_nDragAnchorX, m_nDragAnchorY;
    int  m_bScrollDrag, m_nScrollGrabDY;
    int  m_armDragType, m_armDragIdx, m_armDragItem, m_armDownX, m_armDownY;
    int   m_nLastClickKey; DWORD m_nLastClickTick;

    alignas(8) unsigned char m_ttBuf[0x600];
    bool m_bTtInit; int m_nTtKey;
    IWzFontPtr m_pFont;          // basic font (light) — stack-count numerals on dark badges
    IWzFontPtr m_pFontDk;        // Dotum 11 dark — search text / bag label on the light art

    // --- art / chrome bundled under Custom UI/UIWindow.img/Bag/* (Satchel theme) ---
    IWzCanvasPtr m_pBg;                        // shared background art (STORAGE BAG, 5x5 grid)
    IWzCanvasPtr m_pTabLabel[2][kKindCount];   // tab label glyphs [0]=inactive (tan) [1]=active (brown), per kind
    IWzCanvasPtr m_pPillL[2], m_pPillF[2], m_pPillR[2];  // tab pill 3-slice: [0]=leather(unsel) [1]=parchment(sel)
    IWzCanvasPtr m_pBtClose[2];               // close button: normal, mouseOver
    IWzCanvasPtr m_pBtSort[3];                // sort/merge button: vanilla red BtSort (normal, pressed, mouseOver)
    IWzCanvasPtr m_pBtAuto[2][3];             // AUTO button [off,on][normal, pressed, mouseOver]
    IWzCanvasPtr m_pSbPrev[1], m_pSbNext[1];  // scrollbar up/down arrows
    IWzCanvasPtr m_pSbBase, m_pSbThumb[1];    // scrollbar track tile + thumb
    IWzCanvasPtr m_pDigit[10];                // vanilla ItemNo numerals 0..9 (white, black-outlined) for stack counts
    int          m_digitW[10];                // per-digit advance widths

    // --- search bar (live name filter) ---
    RECT m_rcSearch;
    bool m_searchActive;            // true while the box is focused (keys captured)
    char m_search[kSearchMax + 1];  // typed text, lowercased ASCII
    int  m_searchLen;
    int  m_display[kInvCap];        // when filtering: compacted matching slot indices
    int  m_displayCount;

    CUIBagWindow(int initialKind, int nLeft, int nTop);
    virtual ~CUIBagWindow() override { if (ms_pInstance == this) ms_pInstance = nullptr; }

    virtual void Draw(const RECT* pRect) override;
    virtual void OnMouseButton(unsigned int msg, unsigned int wParam, int rx, int ry) override;
    virtual int  OnMouseMove(int rx, int ry) override;
    virtual int  OnMouseWheel(int rx, int ry, int nWheel) override;
    virtual void OnMouseEnter(int bEnter) override;
    virtual void OnDestroy() override;
    virtual void Update() override { PollDragRelease(); InvalidateRect(nullptr); }
    virtual const CRTTI* GetRTTI() const override { return &ms_RTTI; }
    virtual int IsKindOf(const CRTTI* pRTTI) const override { return ms_RTTI.IsKindOf(pRTTI); }
    virtual int OnSetFocus(int /*bFocus*/) override { return 0; }   // movement-pause fix
    virtual void OnKey(unsigned int wParam, unsigned int lParam) override {
        void* ctx = GetWvsContext();
        if (ctx) reinterpret_cast<int(__thiscall*)(void*, unsigned int, unsigned int)>(
                     kAddr_ProcessBasicUIKey)(ctx, wParam, lParam);
    }

    BagStore& bag() const { return g_bag[m_activeKind]; }

    // Load a canvas by resource path (links auto-resolved; null-safe).
    static IWzCanvasPtr LoadSprite(const wchar_t* p) {
        IWzCanvasPtr c;
        try { c = get_unknown(get_rm()->GetObjectA(const_cast<wchar_t*>(p))); } catch (...) {}
        return c;
    }
    // Opaque blit (background — must cover the world behind it).
    static void BlitAt(IWzCanvasPtr dst, IWzCanvasPtr src, int x, int y) {
        if (dst && src)
            try { dst->CopyEx(x, y, src, CANVAS_ALPHATYPE::CA_REMOVEALPHA, 0, 0, 0, 0, 0, 0); } catch (...) {}
    }
    // Alpha-preserving blit (chrome layered over the opaque background).
    static void BlitA(IWzCanvasPtr dst, IWzCanvasPtr src, int x, int y) {
        if (dst && src)
            try { dst->CopyEx(x, y, src, CANVAS_ALPHATYPE::CA_OVERWRITE, 0, 0, 0, 0, 0, 0); } catch (...) {}
    }
    // Draw a 3-slice tab pill (Bag/tab) at window-local x, total width W: left cap +
    // horizontally-stretched fill + right cap. sel picks the pink (selected) vs grey (unselected) set.
    void DrawTabPill(IWzCanvasPtr dst, int x, int W, bool sel) {
        IWzCanvasPtr L = m_pPillL[sel ? 1 : 0];
        IWzCanvasPtr F = m_pPillF[sel ? 1 : 0];
        IWzCanvasPtr R = m_pPillR[sel ? 1 : 0];
        int lw = 4, rw = 4;
        try { if (L) lw = (int)L->width; } catch (...) {}
        try { if (R) rw = (int)R->width; } catch (...) {}
        int fillW = W - lw - rw; if (fillW < 1) fillW = 1;
        BlitA(dst, L, x, kPillTop);                         // left cap
        if (F && dst)                                        // fill: stretch the 1px column across the middle
            try { dst->CopyEx(x + lw, kPillTop, F, CANVAS_ALPHATYPE::CA_OVERWRITE, fillW, kPillH, 0, 0, 0, 0); } catch (...) {}
        BlitA(dst, R, x + W - rw, kPillTop);                // right cap
    }
    void LoadSprites() {
        m_pBg          = LoadSprite(L"Custom/UI/UIWindow.img/Bag/backgrnd");
        for (int on = 0; on < 2; ++on) {
            const wchar_t* st = on ? L"on" : L"off";
            wchar_t path[80];
            for (int k = 0; k < kKindCount; ++k) {
                swprintf(path, 80, L"Custom/UI/UIWindow.img/Bag/label/%s/%d", st, k);
                m_pTabLabel[on][k] = LoadSprite(path);
            }
            swprintf(path, 80, L"Custom/UI/UIWindow.img/Bag/tab/%s/left", st);  m_pPillL[on] = LoadSprite(path);
            swprintf(path, 80, L"Custom/UI/UIWindow.img/Bag/tab/%s/fill", st);  m_pPillF[on] = LoadSprite(path);
            swprintf(path, 80, L"Custom/UI/UIWindow.img/Bag/tab/%s/right", st); m_pPillR[on] = LoadSprite(path);
        }
        m_pBtClose[0]  = LoadSprite(L"Custom/UI/UIWindow.img/Bag/BtClose/normal/0");
        m_pBtClose[1]  = LoadSprite(L"Custom/UI/UIWindow.img/Bag/BtClose/mouseOver/0");
        // stock item-inventory sort icon, 12x12 (v83 has no Android node; CopyEx blits by top-left, ignoring origin)
        m_pBtSort[0]   = LoadSprite(L"UI/UIWindow.img/Item/BtSort/normal/0");
        m_pBtSort[1]   = LoadSprite(L"UI/UIWindow.img/Item/BtSort/pressed/0");
        m_pBtSort[2]   = LoadSprite(L"UI/UIWindow.img/Item/BtSort/mouseOver/0");
        // AUTO: blue when the tab's auto-collect is on, grey when off
        for (int on = 0; on < 2; ++on) {
            static const wchar_t* kStates[3] = { L"normal", L"pressed", L"mouseOver" };
            for (int st = 0; st < 3; ++st) {
                wchar_t path[80];
                swprintf(path, 80, L"Custom/UI/UIWindow.img/Bag/BtAuto/%s/%s", on ? L"on" : L"off", kStates[st]);
                m_pBtAuto[on][st] = LoadSprite(path);
            }
        }
        // vanilla blue scrollbar (always present in Basic.img)
        m_pSbPrev[0]  = LoadSprite(L"Custom/UI/UIWindow.img/Bag/scroll/prev");
        m_pSbNext[0]  = LoadSprite(L"Custom/UI/UIWindow.img/Bag/scroll/next");
        m_pSbBase     = LoadSprite(L"Custom/UI/UIWindow.img/Bag/scroll/base");
        m_pSbThumb[0] = LoadSprite(L"Custom/UI/UIWindow.img/Bag/scroll/thumb");   // 15x25, 6px caps (3-sliced in Draw)
        // vanilla stack-count digits (white glyph + black outline -> readable on any cell)
        for (int i = 0; i < 10; ++i) {
            wchar_t dp[48]; swprintf(dp, 48, L"UI/Basic.img/ItemNo/%d", i);
            m_pDigit[i] = LoadSprite(dp);
            unsigned int w = 0;
            if (m_pDigit[i]) { try { w = m_pDigit[i]->width; } catch (...) { w = 0; } }
            m_digitW[i] = (w > 0 && w < 32) ? (int)w : ((i == 1) ? 5 : 8);
        }
    }

    // Jump to a specific bag kind (a tab click). Requests that bag's snapshot.
    void SwitchTab(int kind) {
        if (kind < 0 || kind >= kKindCount || kind == m_activeKind) return;
        m_activeKind = kind;
        m_nInvScroll = 0;
        m_nLastClickKey = 0;
        SearchClear();                 // each bag filters independently
        m_nTtKey = 0; HideTip();
        play_ui_sound(L"BtMouseClick");
        SendBagReq_Open(kind);
        InvalidateRect(nullptr);
    }

    // --- search / filtering ---------------------------------------------------
    bool Filtering() const { return m_searchLen > 0; }
    bool MatchItem(int id) const {
        if (!id) return false;
        if (m_searchLen == 0) return true;
        const std::string& n = GetItemNameLower(id);
        if (!n.empty() && strstr(n.c_str(), m_search)) return true;
        // fall back to the item id so unnamed/unknown items stay searchable
        char ids[16]; _snprintf(ids, sizeof(ids), "%d", id); ids[15] = 0;
        return strstr(ids, m_search) != nullptr;
    }
    // Rebuild the compacted list of matching slots (only used while filtering).
    void RebuildFilter() {
        m_displayCount = 0;
        if (!Filtering()) return;
        BagStore& s = bag();
        for (int i = 0; i < s.count && i < kInvCap; ++i)
            if (s.id[i] && MatchItem(s.id[i])) m_display[m_displayCount++] = i;
        ClampScroll();
    }
    // Narrow the already-matching set in place after a character is appended: a longer
    // search term can only shrink the result set, so re-test just the current matches
    // (the prefix already matched the shorter term) instead of rescanning the whole bag.
    void NarrowFilter() {
        if (!Filtering()) { m_displayCount = 0; return; }
        BagStore& s = bag();
        int n = 0;
        for (int g = 0; g < m_displayCount; ++g) {
            int i = m_display[g];
            if (i >= 0 && i < kInvCap && s.id[i] && MatchItem(s.id[i])) m_display[n++] = i;
        }
        m_displayCount = n;
        ClampScroll();
    }
    // Actual bag slot shown at grid position `g` (0-based, row-major), or -1.
    int DisplaySlot(int g) const {
        if (Filtering()) return (g >= 0 && g < m_displayCount) ? m_display[g] : -1;
        return (g >= 0 && g < kInvCap) ? g : -1;
    }
    int TotalRows() const {
        int items = Filtering() ? m_displayCount : kInvCap;
        int rows = (items + kInvCols - 1) / kInvCols;
        return rows < kInvVisRows ? kInvVisRows : rows;
    }
    void SearchClear() { m_searchLen = 0; m_search[0] = 0; m_nInvScroll = 0; RebuildFilter(); }
    void SearchSetActive(bool on) {
        if (on == m_searchActive) return;
        m_searchActive = on;
        if (on) play_ui_sound(L"BtMouseClick");
        InvalidateRect(nullptr);
    }
    // Map a virtual-key to a search char (a-z 0-9 space), or 0. Case-insensitive.
    static char SearchChar(unsigned int vk) {
        if (vk >= 'A' && vk <= 'Z') return (char)('a' + (vk - 'A'));
        if (vk >= '0' && vk <= '9') return (char)vk;
        if (vk == VK_SPACE)         return ' ';
        return 0;
    }
    // Handle a key while the box is focused. Returns true if consumed.
    bool HandleSearchKey(unsigned int vk) {
        if (vk == VK_ESCAPE) { if (m_searchLen) SearchClear(); SearchSetActive(false); return true; }
        if (vk == VK_RETURN) { SearchSetActive(false); return true; }
        if (vk == VK_BACK) {
            if (m_searchLen > 0) { m_search[--m_searchLen] = 0; m_nInvScroll = 0; RebuildFilter(); InvalidateRect(nullptr); }
            return true;
        }
        char ch = SearchChar(vk);
        if (ch) {
            if (m_searchLen < kSearchMax) {
                bool wasFiltering = (m_searchLen > 0);
                m_search[m_searchLen++] = ch; m_search[m_searchLen] = 0; m_nInvScroll = 0;
                // Appending a char can only shrink an existing match set -> narrow in place.
                // The first char (empty -> filtering) needs a full scan to seed m_display.
                if (wasFiltering) NarrowFilter(); else RebuildFilter();
                InvalidateRect(nullptr);
            }
            return true;
        }
        return false;   // not a text key — let the game keep arrow movement etc.
    }

    int  MaxScroll() const { int m = TotalRows() - kInvVisRows; return m > 0 ? m : 0; }
    void ClampScroll() { if (m_nInvScroll < 0) m_nInvScroll = 0; if (m_nInvScroll > MaxScroll()) m_nInvScroll = MaxScroll(); }
    // Track runs between the inset up/down arrows, NOT the raw kScrollTop..kScrollBot.
    void ThumbGeom(int& origin, int& thumbH, int& travel) const {
        int top0 = kScrollTop + kSbArrowH, bot0 = kScrollBot - kSbArrowH;
        int span = bot0 - top0;
        thumbH = span * kInvVisRows / TotalRows();
        if (thumbH < kThumbMinH) thumbH = kThumbMinH;
        if (thumbH > span)       thumbH = span;
        origin = top0;
        travel = span - thumbH;
    }
    void ThumbRect(RECT& rc) const {
        int origin, thumbH, travel;
        ThumbGeom(origin, thumbH, travel);
        int top = origin + (MaxScroll() > 0 ? travel * m_nInvScroll / MaxScroll() : 0);
        rc = { kScrollX, top, kScrollX + kScrollW, top + thumbH };
    }
    static void InvRect(int v, RECT& rc) {
        int col = v % kInvCols, row = v / kInvCols;
        rc.left = kGridLeft + col * kColW;
        rc.top  = kGridTop  + row * kRowH;
        rc.right = rc.left + kCell; rc.bottom = rc.top + kCell;
    }
    // Hit an item cell -> actual bag slot index (or -1). Honors the active filter.
    // Returns -1 while a transfer is in flight: the dense slot indices are stale
    // until the server's reply snapshot lands, so no click/drag/withdraw may act.
    int HitItemSlot(int rx, int ry) const {
        if (s_bAwaitingSnapshot) return -1;
        POINT pt{ rx, ry };
        for (int v = 0; v < kInvVisCount; ++v) {
            RECT rc; InvRect(v, rc);
            if (PtInRect(&rc, pt)) return DisplaySlot(m_nInvScroll * kInvCols + v);
        }
        return -1;
    }

    void EnsureTip() { if (!m_bTtInit) { try { TT_Ctor(m_ttBuf); m_bTtInit = true; } catch (...) { m_bTtInit = false; } } }
    void ShowTip(int x, int y, void* pItem) {
        EnsureTip();
        if (!m_bTtInit || !pItem) return;
        __try { ShowItemToolTip(m_ttBuf, x, y, pItem, nullptr, nullptr, 0, 0, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    void HideTip() { if (m_bTtInit) { __try { TT_Clear(m_ttBuf); } __except (EXCEPTION_EXECUTE_HANDLER) {} m_nTtKey = 0; } }

    // Borrow this window's live HUD origin (COM get_origin can throw — kept in its own
    // C++ try/catch, OUT of BeginItemDrag's __try, which may not mix the two EH models).
    IUnknown* GetOwnOrigin() {
        IUnknown* org = nullptr;
        try { if (m_pLayer) org = static_cast<IUnknown*>(m_pLayer->origin); } catch (...) { org = nullptr; }
        return org;
    }

    // Start a native engine drag from bag slot `slot`.
    void BeginItemDrag(int slot, int itemID, int rx, int ry) {
        if (s_bAwaitingSnapshot) return;   // slot is stale until the reply snapshot
        if (!itemID || !CWndMan::IsInstantiated()) return;
        CWndMan* wm = CWndMan::GetInstance();
        if (!wm) return;
        EndDragIcon();
        // Borrow THIS window's live HUD origin for the drag icon (see CreateDragIconLayer):
        // the window renders correctly, so m_pLayer->origin is a valid origin object.
        IUnknown* pOrigin = GetOwnOrigin();
        IWzGr2DLayer* pIcon = CreateDragIconLayer(itemID, m_screenX + rx, m_screenY + ry, pOrigin);
        if (!pIcon) return;
        void* d = ZAlloc_Alloc(reinterpret_cast<void*>(kZAlloc_Instance), 0x28);   // game ZAlloc (drag manager requires it)
        if (!d) { pIcon->Release(); return; }
        __try {
            ZeroMemory(d, 0x28);
            IDraggable_Init(d, pIcon);
            s_pDragIcon = pIcon;
            *reinterpret_cast<void**>(d)             = reinterpret_cast<void*>(kCDraggableItem_Vtable);
            *reinterpret_cast<int*>((char*)d + 0x18) = 2;       // source type (bag cell)
            *reinterpret_cast<int*>((char*)d + 0x1C) = slot;    // source bag slot index
            *reinterpret_cast<int*>((char*)d + 0x20) = 0;
            *reinterpret_cast<void**>((char*)d + 0x24) = (char*)this + 4;   // source handler
            s_bItemDragging = true;                              // suppress scroll until the drop fires
            BeginDragDrop(wm, (char*)this + 4, d);
            HideTip();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s_bItemDragging = false;
            if (s_pDragIcon == pIcon) s_pDragIcon = nullptr;
        }
    }

    void Withdraw(int slot) {
        if (s_bAwaitingSnapshot) return;   // a transfer is already in flight; index is stale
        if (slot >= 0 && slot < bag().count && bag().id[slot]) {
            SendBagReq_Withdraw(m_activeKind, slot);
            play_ui_sound(L"DragEnd"); HideTip();
        }
    }

    // Complete a press-drag-RELEASE. This client's CWndMan ends an engine drag only on the
    // NEXT click (WM_LBUTTONDOWN/DBLCLK), never on button-up, so releasing over the inventory
    // would otherwise leave the withdrawn item stuck to the cursor. Called every frame
    // from Update(): once one of OUR bag drags is in flight (CWndMan+0x90 == our handler)
    // and the left button is up, we finish the drag via CWndMan::EndDragDrop, which fires
    // CDraggableItem::OnDropped (our hook) exactly like a click-drop — routing to Withdraw
    // when dropped outside the bag, or a no-op when dropped back on it.
    void PollDragRelease() {
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) return; // button physically still held -> not released yet
        if (!CWndMan::IsInstantiated()) return;
        CWndMan* wm = CWndMan::GetInstance();
        if (!wm) return;
        void* ourHandler = reinterpret_cast<char*>(this) + 4;
        void* src = nullptr;
        __try { src = *reinterpret_cast<void**>(reinterpret_cast<char*>(wm) + 0x90); }
        __except (EXCEPTION_EXECUTE_HANDLER) { src = nullptr; }
        if (src != ourHandler) return;                     // no drag of ours in flight
        // EndDragDrop wants the cursor relative to the window under it (CWndMan::ProcessMouse
        // 0x009E3B3E-0x009E3B52 subtracts that window's GetAbsLeft/Top). CWndMan+0x9C/+0xA0 is
        // the last mouse position and +0x8C the handler under it. Our OnDropped hook ignores
        // rx/ry for bag drags; the hover target's OnDraggableMove still sees them.
        int rx = 0, ry = 0;
        __try {
            rx = *reinterpret_cast<int*>(reinterpret_cast<char*>(wm) + 0x9C);
            ry = *reinterpret_cast<int*>(reinterpret_cast<char*>(wm) + 0xA0);
            IUIMsgHandler* pHover = *reinterpret_cast<IUIMsgHandler**>(reinterpret_cast<char*>(wm) + 0x8C);
            if (pHover) { rx -= pHover->GetAbsLeft(); ry -= pHover->GetAbsTop(); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { rx = 0; ry = 0; }
        s_bItemDragging = false;                           // OnDropped will also clear this
        __try { EndDragDrop(wm, rx, ry, 0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        EndDragIcon();
    }
};

// Keep a window fully on-screen when restoring a remembered position (a
// resolution change since last close could otherwise leave it off-screen).
static void ClampBagToScreen(int& x, int& y) {
    int sw = get_screen_width(), sh = get_screen_height();
    if (x < 8) x = 8;
    if (sw > kWndW && x > sw - kWndW) x = sw - kWndW;
    if (y < 0) y = 0;
    if (sh > kWndH && y > sh - kWndH) y = sh - kWndH;
}

// Create the bag window (if absent) focused on `initialKind`. Reuses the
// remembered position, else centers it.
static CUIBagWindow* EnsureBagWindow(int initialKind) {
    if (CUIBagWindow::ms_pInstance) return CUIBagWindow::ms_pInstance;
    if (initialKind < 0 || initialKind >= kKindCount) initialKind = kKindOre;
    int x, y;
    if (s_bSavedPos) { x = s_savedX; y = s_savedY; }
    else { x = (get_screen_width() - kWndW) / 2; y = 110; }
    ClampBagToScreen(x, y);
    return new CUIBagWindow(initialKind, x, y);
}

CUIBagWindow::CUIBagWindow(int initialKind, int nLeft, int nTop)
    : m_activeKind(initialKind), m_nInvScroll(0), m_screenX(nLeft), m_screenY(nTop),
      m_nCloseHover(0), m_nMergeHover(0), m_nMergePressed(0), m_nAutoHover(0), m_nAutoPressed(0),
      m_bDragging(0), m_nDragAnchorX(0), m_nDragAnchorY(0),
      m_bScrollDrag(0), m_nScrollGrabDY(0),
      m_armDragType(0), m_armDragIdx(0), m_armDragItem(0), m_armDownX(0), m_armDownY(0),
      m_nLastClickKey(0), m_nLastClickTick(0), m_bTtInit(false), m_nTtKey(0),
      m_searchActive(false), m_searchLen(0), m_displayCount(0) {
    m_search[0] = 0;
    m_rcClose  = { kBtCloseX,  kBtCloseY,  kBtCloseX  + kBtCloseW,  kBtCloseY  + kBtCloseH };
    m_rcMerge  = { kBtMergeX,  kBtMergeY,  kBtMergeX  + kBtMergeW,  kBtMergeY  + kBtMergeH };
    m_rcAuto   = { kBtAutoX,   kBtAutoY,   kBtAutoX   + kBtAutoW,   kBtAutoY   + kBtAutoH };
    for (int k = 0; k < kKindCount; ++k)
        m_rcTab[k] = { kTabHitLeft[k], kTabTop, kTabHitRight[k], kTabBot };
    m_rcSearch = { kSearchBoxL, kSearchBoxTop, kSearchBoxR, kSearchBoxBot };
    ms_pInstance = this;
    LoadSprites();
    CWnd::CreateWnd(this, nLeft, nTop, kWndW, kWndH, 10, 1, nullptr, 0);
    StackRaise(this);                // join the Esc close stack, as stock windows do in OnCreate
    play_ui_sound(L"MenuUp");
    m_pFont = nullptr;
    try { get_basic_font(std::addressof(m_pFont), 0); } catch (...) {}
    // Dark Dotum 11 for text drawn on the light art (search box / bag-name label).
    m_pFontDk = nullptr;
    try {
        PcCreateObject<IWzFontPtr>(L"Canvas#Font", m_pFontDk, nullptr);
        if (m_pFontDk) {
            HRESULT hr = reinterpret_cast<HRESULT(__thiscall*)(IWzFont*, Ztl_bstr_t, unsigned long,
                unsigned long, const Ztl_variant_t&)>(kAddr_SetFont)(
                m_pFontDk, L"Dotum", 11, kSatchelInk, Ztl_variant_t(L""));
            if (FAILED(hr)) m_pFontDk = nullptr;
        }
    } catch (...) { m_pFontDk = nullptr; }
    s_bAwaitingSnapshot = false;     // a fresh window starts un-gated (no transfer in flight yet)
    SendBagReq_Open(m_activeKind);   // request the current snapshot for the active bag
}

void CUIBagWindow::OnDestroy() {
    // Remember placement + active bag so the next open restores them.
    s_savedX = m_screenX; s_savedY = m_screenY; s_bSavedPos = true; s_savedKind = m_activeKind;
    if (!s_bLeavingField) SendBagReq_Close();
    // Leave the Esc stack BEFORE CWnd::OnDestroy, as stock OnDestroy does; a stale entry would be
    // handed to TryCloseUI after this object is freed.
    StackRemove(this);
    if (m_bTtInit) { __try { TT_Clear(m_ttBuf); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    for (int k = 0; k < kKindCount; ++k) FreeBag(k);
    if (m_bTtInit) { __try { TT_Dtor(m_ttBuf); } __except (EXCEPTION_EXECUTE_HANDLER) {} m_bTtInit = false; }
    m_pFont = nullptr; m_pFontDk = nullptr;
    m_pBg = nullptr;
    for (int on = 0; on < 2; ++on) for (int k = 0; k < kKindCount; ++k) m_pTabLabel[on][k] = nullptr;
    for (int i = 0; i < 2; ++i) { m_pPillL[i] = nullptr; m_pPillF[i] = nullptr; m_pPillR[i] = nullptr; }
    m_pBtClose[0] = nullptr; m_pBtClose[1] = nullptr;
    m_pBtSort[0] = nullptr; m_pBtSort[1] = nullptr; m_pBtSort[2] = nullptr;
    for (int on = 0; on < 2; ++on) for (int st = 0; st < 3; ++st) m_pBtAuto[on][st] = nullptr;
    m_pSbPrev[0] = nullptr; m_pSbNext[0] = nullptr; m_pSbBase = nullptr; m_pSbThumb[0] = nullptr;
    for (int i = 0; i < 10; ++i) m_pDigit[i] = nullptr;
    if (ms_pInstance == this) ms_pInstance = nullptr;
    CWnd::OnDestroy();
}

// Pixel width of `s` in `font`. A trailing space is added by hand in case the measure trims it,
// so the caret still advances after typing one.
static int TextWidth(IWzFont* font, const char* s) {
    if (!font || !s || !*s) return 0;
    int w = 0;
    try { w = (int)font->CalcTextWidth(Ztl_bstr_t(s), Ztl_variant_t()); } catch (...) { return 0; }
    size_t n = strlen(s);
    if (s[n - 1] == ' ') {
        try {
            int wTrim = (int)font->CalcTextWidth(Ztl_bstr_t("a a"), Ztl_variant_t())
                      - (int)font->CalcTextWidth(Ztl_bstr_t("aa"), Ztl_variant_t());
            int wLast = w - (int)font->CalcTextWidth(Ztl_bstr_t(std::string(s, n - 1).c_str()), Ztl_variant_t());
            if (wLast <= 0) w += wTrim;   // the measure dropped the trailing space
        } catch (...) {}
    }
    return w;
}

void CUIBagWindow::Draw(const RECT* pRect) {
    CWnd::Draw(pRect);
    IWzCanvasPtr pCanvas = GetCanvas();
    if (!pCanvas) return;
    auto pItemInfo = CItemInfo::GetInstance();
    IWzFont* pf   = m_pFont;
    IWzFont* pfDk = m_pFontDk ? (IWzFont*)m_pFontDk : pf;

    // (1) Shared background — title, the 5x5 grid wells and the search-box frame are baked into
    //     the art (UI/UIWindow.img/Bag/backgrnd). The active bag is shown by the tab labels, not the bg.
    BlitAt(pCanvas, m_pBg, 0, 0);
    if (!m_pBg) {
        try {
            pCanvas->DrawRectangle(0, 0, kWndW, kWndH, 0xFFF4E8C8);
            pCanvas->DrawRectangle(0, 0, kWndW, kTitleH, 0xFFC9A06A);
            pCanvas->DrawRectangle(0, kTitleH, kWndW, 1, 0xFF8B5A2B);
            for (int v = 0; v < kInvVisCount; ++v) {
                RECT rc; InvRect(v, rc);
                pCanvas->DrawRectangle(rc.left, rc.top, kCell, kCell, 0xFFEAD7A8);
            }
            pCanvas->DrawRectangle(kSearchBoxL, kSearchBoxTop,
                kSearchBoxR - kSearchBoxL, kSearchBoxBot - kSearchBoxTop, 0xFFFFFFFF);
            if (pfDk) {
                pCanvas->DrawTextA(6, 4, Ztl_bstr_t(L"STORAGE BAG"), pfDk, Ztl_variant_t(), Ztl_variant_t());
            }
        } catch (...) {}
    }

    // (1b) Tabs in the strip below the title — vanilla item-inventory style: a pink pill behind the
    //      active kind, grey pills behind the rest, each with its white label centered on top.
    for (int k = 0; k < kKindCount; ++k) {
        bool sel = (k == m_activeKind);
        DrawTabPill(pCanvas, kTabHitLeft[k] + kPillPad, (kTabHitRight[k] - kTabHitLeft[k]) - 2 * kPillPad, sel);
        // label centred in its slot; inactive pills sit 2px lower, so their label drops 1px
        IWzCanvasPtr lab = m_pTabLabel[sel ? 1 : 0][k];
        int lw = 0;
        try { if (lab) lw = (int)lab->width; } catch (...) {}
        BlitA(pCanvas, lab, kTabHitLeft[k] + ((kTabHitRight[k] - kTabHitLeft[k]) - lw) / 2, kTabLabelY + (sel ? 0 : 1));
    }

    // (2) Item icons over the baked grid cells, with the per-slot stack count drawn
    //     bottom-left like the vanilla inventory (white numerals on a dark badge).
    BagStore& s = bag();
    for (int v = 0; v < kInvVisCount; ++v) {
        int slot = DisplaySlot(m_nInvScroll * kInvCols + v);
        if (slot >= 0 && slot < kInvCap && s.ready && s.id[slot]) {
            RECT rc; InvRect(v, rc);
            if (pItemInfo)
                pItemInfo->DrawItemIconForSlot(pCanvas, s.id[slot], rc.left + kIconDX, rc.top + kIconBY, 0, 0, 0, 0, 0, 0);
            // per-slot stack count — vanilla ItemNo digits (white glyph + black outline,
            // no box) seated at the cell's bottom-left and nudged up so they don't clip.
            int q = s.qty[slot];
            if (q >= 1) {
                char num[12]; _snprintf(num, sizeof(num), "%d", q); num[11] = 0;
                int dx = rc.left + 1, dy = rc.bottom - 14;
                for (const char* np = num; *np; ++np) {
                    int d = *np - '0';
                    if (d >= 0 && d <= 9 && m_pDigit[d]) { BlitA(pCanvas, m_pDigit[d], dx, dy); dx += m_digitW[d]; }
                }
            }
        }
    }

    // (3) Vertical scrollbar (vanilla blue VScr4) in the right margin — only when
    //     the list overflows. base + thumb are tiles, stretched to fill.
    if (MaxScroll() > 0) {
        auto stretchV = [&](IWzCanvasPtr sp, int y, int h) {
            if (sp && h > 0) try { pCanvas->CopyEx(kScrollX, y, sp, CANVAS_ALPHATYPE::CA_OVERWRITE,
                                                   kScrollW, h, 0, 0, 0, 0); } catch (...) {}
        };
        int trackY = kScrollTop + kSbArrowH, trackH = (kScrollBot - kSbArrowH) - trackY;
        stretchV(m_pSbBase, trackY, trackH);                              // track groove
        BlitA(pCanvas, m_pSbPrev[0], kScrollX, kScrollTop);               // up arrow
        BlitA(pCanvas, m_pSbNext[0], kScrollX, kScrollBot - kSbArrowH);   // down arrow
        // thumb: vanilla VScr4 grip (25px source) 3-sliced — rounded caps + stretched middle
        RECT th; ThumbRect(th);
        IWzCanvasPtr thumb = m_pSbThumb[0];
        const int srcH = 25, cap = 6;
        int thH = th.bottom - th.top;
        if (thumb) {
            if (thH < 2 * cap + 2) { stretchV(thumb, th.top, thH); }
            else try {
                pCanvas->CopyEx(kScrollX, th.top,          thumb, CANVAS_ALPHATYPE::CA_OVERWRITE, kScrollW, cap,           0, 0,          15, cap);
                pCanvas->CopyEx(kScrollX, th.top + cap,    thumb, CANVAS_ALPHATYPE::CA_OVERWRITE, kScrollW, thH - 2 * cap, 0, cap,        15, srcH - 2 * cap);
                pCanvas->CopyEx(kScrollX, th.bottom - cap, thumb, CANVAS_ALPHATYPE::CA_OVERWRITE, kScrollW, cap,           0, srcH - cap, 15, cap);
            } catch (...) {}
        }
    }

    // (5) Sort/merge button (vanilla red BtSort): consolidate identical stacks.
    //     pressed > hover > normal art.
    BlitA(pCanvas, m_nMergePressed ? m_pBtSort[1] : (m_nMergeHover ? m_pBtSort[2] : m_pBtSort[0]),
          m_rcMerge.left, m_rcMerge.top);

    // (5b) AUTO button: the art shows the server-reported state for the active tab.
    {
        const int on = s_bAutoOn[m_activeKind] ? 1 : 0;
        BlitA(pCanvas, m_nAutoPressed ? m_pBtAuto[on][1] : (m_nAutoHover ? m_pBtAuto[on][2] : m_pBtAuto[on][0]),
              m_rcAuto.left, m_rcAuto.top);
    }

    // (6) Close button — mouseOver art when hovered.
    BlitA(pCanvas, m_nCloseHover ? m_pBtClose[1] : m_pBtClose[0], m_rcClose.left, m_rcClose.top);

    // (7) Search field — drawn ON TOP of the baked search box at the bottom: typed
    //     text (or a dim hint), a blinking caret while focused, and a result-count.
    if (pfDk) {
        if (m_searchLen > 0) {
            try { pCanvas->DrawTextA(kSearchTextX, kSearchTextY, Ztl_bstr_t(m_search), pfDk, Ztl_variant_t(), Ztl_variant_t()); } catch (...) {}
        } else if (!m_searchActive) {
            static const wchar_t* kHints[kKindCount] =
                { L"Search ores...", L"Search scrolls...", L"Search chairs...", L"Search cash items..." };
            const wchar_t* hint = kHints[m_activeKind];
            try { pCanvas->DrawTextA(kSearchTextX, kSearchTextY, Ztl_bstr_t(hint), pfDk, Ztl_variant_t(), Ztl_variant_t()); } catch (...) {}
        }
        // caret (blink ~500ms) right after the text
        if (m_searchActive && ((GetTickCount() / 500) & 1) == 0) {
            int cx = kSearchTextX + TextWidth(pfDk, m_search) + 1;   // measured: Dotum is proportional
            pCanvas->DrawRectangle(cx, kSearchTextY, 1, 12, kSatchelInk);
        }
        // result-count badge when filtering
        if (Filtering()) {
            char cnt[16]; _snprintf(cnt, sizeof(cnt), "%d", m_displayCount); cnt[15] = 0;
            int cw = TextWidth(pfDk, cnt);
            // left of the baked magnifier
            try { pCanvas->DrawTextA(kSearchBoxR - cw - 20, kSearchTextY, Ztl_bstr_t(cnt), pfDk, Ztl_variant_t(), Ztl_variant_t()); } catch (...) {}
        }
    }
}

void CUIBagWindow::OnMouseButton(unsigned int msg, unsigned int wParam, int rx, int ry) {
    POINT pt{ rx, ry };
    if (msg == WM_LBUTTONDOWN) {
        StackRaise(this);                  // a click makes the bag the next thing Esc closes
        if (PtInRect(&m_rcSearch, pt)) { SearchSetActive(true); return; }  // focus the search box
        SearchSetActive(false);            // clicking anything else unfocuses it
        if (PtInRect(&m_rcClose, pt)) { Destroy(); return; }
        for (int k = 0; k < kKindCount; ++k) {
            if (PtInRect(&m_rcTab[k], pt)) { SwitchTab(k); return; }   // tab click: load that bag kind
        }
        if (PtInRect(&m_rcAuto, pt)) {    // AUTO: flip this tab's pickup auto-collect
            m_nAutoPressed = 1; SendBagReq_SetAuto(m_activeKind, !s_bAutoOn[m_activeKind]); play_ui_sound(L"BtMouseClick");
            return;
        }
        if (PtInRect(&m_rcMerge, pt)) {   // sort/merge: ask the server to consolidate stacks
            m_nMergePressed = 1; SendBagReq_Merge(m_activeKind); play_ui_sound(L"BtMouseClick");
            return;
        }
        // scrollbar (ignored while ANY item drag is in flight — bag drag-out or a
        // drag in/over the window from the inventory; only a no-drag grab scrolls)
        if (!ScrollLocked() && MaxScroll() > 0 && rx >= kScrollX && rx < kScrollX + kScrollW && ry >= kScrollTop && ry < kScrollBot) {
            if (ry < kScrollTop + kSbArrowH) { m_nInvScroll--; ClampScroll(); return; }
            if (ry >= kScrollBot - kSbArrowH) { m_nInvScroll++; ClampScroll(); return; }
            RECT th; ThumbRect(th);
            if (PtInRect(&th, pt)) { m_bScrollDrag = 1; m_nScrollGrabDY = ry - th.top; }
            return;
        }
        // item cell: double-click withdraws; first click arms a drag
        int slot = HitItemSlot(rx, ry);
        if (slot >= 0 && slot < bag().count && bag().id[slot]) {
            int key = slot + 1;
            DWORD now = GetTickCount();
            if (key == m_nLastClickKey && (now - m_nLastClickTick) <= GetDoubleClickTime()) {
                Withdraw(slot); m_nLastClickKey = 0; return;
            }
            m_nLastClickKey = key; m_nLastClickTick = now;
            m_armDragType = 2; m_armDragIdx = slot; m_armDragItem = bag().id[slot];
            m_armDownX = rx; m_armDownY = ry;
            return;
        }
        if (ry < kTitleH) { m_bDragging = 1; m_nDragAnchorX = rx; m_nDragAnchorY = ry; }
    } else if (msg == WM_RBUTTONDOWN) {
        int slot = HitItemSlot(rx, ry);
        if (slot >= 0) Withdraw(slot);         // right-click = quick withdraw
    } else if (msg == WM_LBUTTONUP) {
        m_bDragging = 0; m_bScrollDrag = 0; m_armDragType = 0; s_bItemDragging = false;
        if (m_nMergePressed) { m_nMergePressed = 0; InvalidateRect(nullptr); }
        if (m_nAutoPressed) { m_nAutoPressed = 0; InvalidateRect(nullptr); }
    }
    CWnd::OnMouseButton(msg, wParam, rx, ry);
}

int CUIBagWindow::OnMouseMove(int rx, int ry) {
    if (m_bDragging) {
        int dx = rx - m_nDragAnchorX, dy = ry - m_nDragAnchorY;
        if ((dx || dy) && m_pLayer) {
            m_pLayer->RelOffset(dx, dy, Ztl_variant_t(), Ztl_variant_t());
            m_screenX += dx; m_screenY += dy;
        }
        HideTip(); return 1;
    }
    if (m_bScrollDrag) {
        int origin, thumbH, travel;
        ThumbGeom(origin, thumbH, travel);
        int rel = (ry - m_nScrollGrabDY) - origin;
        m_nInvScroll = (travel > 0) ? (rel * MaxScroll() + travel / 2) / travel : 0;
        ClampScroll(); HideTip(); return 1;
    }
    // promote an armed click into a real engine drag once it moves past a dead-zone.
    // GetAsyncKeyState (real-time physical state), NOT GetKeyState: in this client
    // GetKeyState(VK_LBUTTON) does not report the held mouse button during a drag-move,
    // so the arm was disarming on the first move and the drag never started at all.
    if (m_armDragType) {
        if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) { m_armDragType = 0; }
        else {
            int dx = rx - m_armDownX, dy = ry - m_armDownY;
            if (dx * dx + dy * dy > 16) {
                int slot = m_armDragIdx, item = m_armDragItem;
                m_armDragType = 0; m_nLastClickKey = 0;
                BeginItemDrag(slot, item, rx, ry);
                return 1;
            }
        }
    }
    // hover: close box + switch + sort/merge buttons
    POINT pt{ rx, ry };
    m_nCloseHover = PtInRect(&m_rcClose, pt) ? 1 : 0;
    int mgHov = PtInRect(&m_rcMerge, pt) ? 1 : 0;
    if (mgHov != m_nMergeHover) { m_nMergeHover = mgHov; if (mgHov) play_ui_sound(L"BtMouseOver"); InvalidateRect(nullptr); }
    int auHov = PtInRect(&m_rcAuto, pt) ? 1 : 0;
    if (auHov != m_nAutoHover) { m_nAutoHover = auHov; if (auHov) play_ui_sound(L"BtMouseOver"); InvalidateRect(nullptr); }
    // hover tooltip
    int slot = HitItemSlot(rx, ry);
    int key = (slot >= 0 && slot < bag().count && bag().id[slot]) ? (slot + 1) : 0;
    if (key != m_nTtKey) {
        if (key) {
            GW_ItemSlotBase* p = (slot >= 0 && slot < kInvCap) ? (GW_ItemSlotBase*)bag().obj[slot] : nullptr;
            if (p) { ShowTip(m_screenX + rx + 12, m_screenY + ry, p); m_nTtKey = key; }
            else { HideTip(); }
        } else HideTip();
    }
    return 1;
}

int CUIBagWindow::OnMouseWheel(int rx, int ry, int nWheel) {
    if (ScrollLocked()) return 1;                // don't scroll while dragging an item (in OR out)
    m_nInvScroll += (nWheel > 0) ? 1 : -1;       // wheel up -> earlier rows
    ClampScroll(); HideTip(); return 1;
}

void CUIBagWindow::OnMouseEnter(int bEnter) {
    CWnd::OnMouseEnter(bEnter);
    if (!bEnter) { m_nCloseHover = 0; m_nMergeHover = 0; m_nAutoHover = 0; m_nAutoPressed = 0; HideTip(); }
}

// Parse a snapshot into g_bag[kind]. RESP_SNAPSHOT opens the window if closed and focuses
// the snapshot's bag (covers @orebag); RESP_REFRESH only updates the cache and redraws when
// that bag is the one shown. A refresh for the shown bag keeps the scroll position.
static void HandleBagSnapshot(CInPacket* pkt, bool bShow) {
    int kind = pkt->Decode<unsigned char>();
    if (kind < 0 || kind >= kKindCount) return;
    // Only a reply ends an in-flight transfer: a pushed refresh can arrive before it, and the
    // indices it carries would still shift when the transfer's reply lands.
    if (bShow) s_bAwaitingSnapshot = false;
    FreeBag(kind);
    int count = pkt->Decode<short>();               // item count (short — capacity can exceed a byte, max 200)
    if (count < 0) count = 0;
    if (count > kInvCap) count = kInvCap;
    short itemSlots[kInvCap];                       // each item's slot, in wire order
    int maxSlot = 0;
    int decoded = 0;                                // items actually parsed (for the qty block)
    for (int i = 0; i < count; ++i) {
        if (!pkt->CanRead(2)) break;                // no room left for even the slot short -> truncated
        short slot = pkt->Decode<short>();
        GW_ItemSlotBase* p = RawDecodeItem(pkt);    // engine decode, shares the packet offset
        if (!p || !pkt->CanRead(0)) { DropRef(p); break; }   // decode failed or overran m_uLength
        itemSlots[decoded++] = slot;
        if (slot >= 0 && slot < kInvCap) {
            StoreDecoded(g_bag[kind].obj[slot], p);
            g_bag[kind].id[slot] = DecodeItemID(p);
            if (slot + 1 > maxSlot) maxSlot = slot + 1;
        } else DropRef(p);
    }
    // Trailing stack-count block (one short per item, same order). Absent on older
    // servers -> CanRead fails -> quantities stay 0 (no number drawn).
    for (int i = 0; i < decoded; ++i) {
        short q = pkt->Decode<short>();
        short slot = itemSlots[i];
        if (slot >= 0 && slot < kInvCap) g_bag[kind].qty[slot] = q;
    }
    // Trailing auto-collect byte. Only trusted when every item decoded, since the blocks above
    // are then aligned; an older server omits it and the state is left as it was.
    if (decoded == count && pkt->CanRead(1)) s_bAutoOn[kind] = pkt->Decode<unsigned char>() != 0;
    g_bag[kind].count = maxSlot;
    g_bag[kind].ready = true;
    CUIBagWindow* w = bShow ? EnsureBagWindow(kind) : CUIBagWindow::ms_pInstance;
    if (w && (bShow || w->m_activeKind == kind)) {
        if (w->m_activeKind != kind) { w->m_activeKind = kind; w->m_nInvScroll = 0; }
        w->RebuildFilter();                    // item set changed -> refresh the filtered view
        w->ClampScroll();                      // a withdraw can shrink the list past the offset
        w->InvalidateRect(nullptr);
    }
}

// Key Config shortcut / BAG button: toggle the window.
static void ToggleBags() {
    if (CUIBagWindow::ms_pInstance) { CUIBagWindow::ms_pInstance->Destroy(); return; }
    EnsureBagWindow(s_savedKind);
}

// The window refuses focus (OnSetFocus returns 0, so the player keeps moving), so keys reach
// CField::OnKey. While the search box is active, it claims text keys there first.
static bool OnFieldKey(unsigned int wParam, int lParam) {
    const bool isKeyUp = (lParam & 0x80000000) != 0;
    CUIBagWindow* w = CUIBagWindow::ms_pInstance;
    if (w && w->m_searchActive && !isKeyUp) {
        if (w->HandleSearchKey(wParam)) return true;
    }
    return false;
}

// Called for every CDraggableItem drop. pThis is the draggable (+0x18 type, +0x1C slot,
// +0x24 source handler), pTo the IUIMsgHandler it landed on (a CWnd's +4 subobject), or null.
// Returns true when the bag consumed the drop and the original must not run: a bag drag
// carries type 2 + a BAG slot, which the stock code would read as an inventory move.
static bool HandleDropped(void* pThis, void* /*pFrom*/, void* pTo, int /*rx*/, int /*ry*/) {
    s_bItemDragging = false;
    CUIBagWindow* w = CUIBagWindow::ms_pInstance;
    if (!pThis || !w) return false;
    void* srcHandler = nullptr; int srcA = 0, srcB = 0;
    __try {
        srcHandler = *reinterpret_cast<void**>(reinterpret_cast<char*>(pThis) + 0x24);
        srcA = *reinterpret_cast<int*>(reinterpret_cast<char*>(pThis) + 0x18);
        srcB = *reinterpret_cast<int*>(reinterpret_cast<char*>(pThis) + 0x1C);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    void* ourHandler = reinterpret_cast<char*>(w) + 4;
    if (srcHandler == ourHandler) {
        if (!(pTo == ourHandler || pTo == static_cast<void*>(w)))
            w->Withdraw(srcB);
        EndDragIcon();
        return true;
    }
    if (pTo == ourHandler || pTo == static_cast<void*>(w)) {
        // Only inventory drags carry (inventory type 1..5, slot); the server re-validates anyway.
        if (srcA >= 1 && srcA <= 5) {
            SendBagReq_Deposit(w->m_activeKind, srcA, srcB);
            play_ui_sound(L"DragEnd");
        }
        return true;
    }
    return false;
}

// Opcode (2) + respType (1) + payload.
static void HandlePacket(CInPacket* pPacket) {
    pPacket->Decode<unsigned short>();                 // the opcode; the dispatcher only Peek2s it
    if (!pPacket->CanRead(1)) return;
    int respType = pPacket->Decode<unsigned char>();
    if (respType == kResp_Snapshot || respType == kResp_Refresh) HandleBagSnapshot(pPacket, respType == kResp_Snapshot);
}

// ===========================================================================
// Inventory-window bag button.
//
// A real engine CCtrlButton injected onto the vanilla ITEM inventory window
// (CUIItem), one slot left of the expand button, that toggles the storage bag on
// click. A drawn overlay can't be clicked in that title-bar strip (clicks there
// are captured for window-dragging), so we inject a genuine child button and let
// the engine hit-test it; the click arrives as CUIItem::OnChildNotify(nId,0x64,..).
//
// Addresses (checked against v83.idb / this exe):
//   CUIItem::OnCreate       0x0081C6C9  (ret 4; add our button after the original runs)
//   CUIItem::OnChildNotify  0x0081D01F  (ret 0xC; code 0x64 = click at 0x0081D20F, 0x65 = hover at 0x0081D09F)
//   CCtrlButton ctor        0x004258E4 ; allocator = ZAlloc_Alloc(kZAlloc_Instance) (reused from above)
//   CCtrlButton::CreateCtrl == primary vtable 0x00AF0C10 [+0x20] = 0x004BFFFB (ret 0x18, 6 args)
// ===========================================================================
static constexpr uintptr_t kAddr_CUIItem_OnCreate      = 0x0081C6C9;
static constexpr uintptr_t kAddr_CUIItem_OnChildNotify = 0x0081D01F;
static constexpr uintptr_t kAddr_CCtrlButton_ctor      = 0x004258E4;
static constexpr int kCCtrlButton_Size    = 0x5A4;
static constexpr int kVtbl_CreateCtrl_Off = 0x20;

static constexpr unsigned int kInvBtnId       = 0x8888;   // free id (vanilla item window uses 0x7D0..0x7D7)
static constexpr int          kInvNotifyClick = 0x64;     // OnChildNotify code for a click (0x65 = hover enter/leave)

// Button position within the window (window-local): one slot left of the expand
// button (BtFull), which the client creates at (m_width - 30, 6) == (125, 6).
static int g_invBtnX = 97;
static int g_invBtnY = 6;

// SEH-isolated engine calls — each __try lives in its own function with no C++
// object needing unwinding (MSVC C2712 forbids mixing the two).
static void* SehBtnAlloc(size_t n) {
    void* p = nullptr;
    __try { p = ZAlloc_Alloc(reinterpret_cast<void*>(kZAlloc_Instance), n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { p = nullptr; }
    return p;
}
static bool SehBtnCtor(void* mem) {
    bool ok = true;
    __try { reinterpret_cast<void(__thiscall*)(void*)>(kAddr_CCtrlButton_ctor)(mem); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}
static bool SehBtnCreateCtrl(void* btn, void* parent, unsigned int id, int x, int y, void* param) {
    bool ok = true;
    __try {
        void** vtbl = *reinterpret_cast<void***>(btn);
        auto pfn = reinterpret_cast<void(__thiscall*)(void*, void*, unsigned int, int, int, int, void*)>(
            vtbl[kVtbl_CreateCtrl_Off / sizeof(void*)]);
        pfn(btn, parent, id, x, y, 0, param);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}

static void BtnWiden(const char* a, wchar_t* w, size_t cap) {
    size_t i = 0;
    if (a) for (; a[i] && i + 1 < cap; ++i) w[i] = (wchar_t)(unsigned char)a[i];
    w[i] = 0;
}

// Our button, ref-held while the inventory window lives. The engine also holds a
// child ref and releases it on window destroy; this ZRef keeps the object valid
// until the next OnCreate releases + recreates it.
static ZRef<CCtrlButton> s_invBtn;
static void* s_invBtnParent = nullptr;   // the CUIItem instance our button belongs to (dedupe guard)

// TSingleton<CUIItem>::ms_pInstance, written by the CUIItem ctor at 0x0081C455.
static constexpr uintptr_t kAddr_CUIItem_Instance = 0x00BED654;

static void CreateInvButton(void* pParentWnd) {
    // Dedupe: CUIItem::OnCreate can re-fire on the SAME live window (e.g. inventory expand/collapse).
    // We only drop our OWN ref on recreate, but the engine keeps the previous button parented, so a
    // second add would leave two "BAG" buttons on the window. Skip if this window already has ours.
    if (pParentWnd && pParentWnd == s_invBtnParent) return;
    s_invBtn = ZRef<CCtrlButton>();                    // release any button from a previous window
    s_invBtnParent = nullptr;
    void* mem = SehBtnAlloc(kCCtrlButton_Size);
    if (!mem) return;
    if (!SehBtnCtor(mem)) return;                      // engine CCtrlButton ctor (refcount 0)
    CCtrlButton* btn = reinterpret_cast<CCtrlButton*>(mem);
    s_invBtn = ZRef<CCtrlButton>(btn);                 // AddRef 0 -> 1

    wchar_t wuol[128];
    BtnWiden("Custom/UI/UIWindow.img/Bag/BtOreBag", wuol, 128);   // custom 25x12 art (origin 19,6), Custom.wz
    CCtrlButton::CREATEPARAM param;                    // ctor zeroes flags; sUOL default-constructed
    param.sUOL = wuol;
    if (!SehBtnCreateCtrl(btn, pParentWnd, kInvBtnId, g_invBtnX, g_invBtnY, &param)) {
        s_invBtn = ZRef<CCtrlButton>();                // creation failed -> drop it
    } else {
        s_invBtnParent = pParentWnd;                   // remember the owning window
    }
}

// CUIItem::OnCreate — build the window normally, then add our child button.
typedef void(__thiscall* t_CUIItem_OnCreate)(void*, void*);
static auto CUIItem_OnCreate = reinterpret_cast<t_CUIItem_OnCreate>(kAddr_CUIItem_OnCreate);
void __fastcall CUIItem_OnCreate_hook(void* pThis, void* /*edx*/, void* pData) {
    CUIItem_OnCreate(pThis, pData);
    // Only the canonical inventory SINGLETON (TSingleton @0x00BED654) gets the BAG button. The client
    // constructs a transient/secondary CUIItem during init whose OnCreate also fires here; adding a
    // button to it left a lingering second "BAG" button. Gate on the singleton so exactly one window
    // (the one the player actually sees) ever carries the button. Fallback: if the instance pointer
    // isn't set yet, still add (so we never end up with zero buttons).
    void* singleton = nullptr;
    __try { singleton = *reinterpret_cast<void**>(kAddr_CUIItem_Instance); } __except (EXCEPTION_EXECUTE_HANDLER) { singleton = nullptr; }
    if (singleton == nullptr || pThis == singleton) CreateInvButton(pThis);
}

// CUIItem::OnChildNotify — catch our button's click (our id + code 0x64).
typedef void(__thiscall* t_CUIItem_OnChildNotify)(void*, unsigned int, unsigned int, unsigned int);
static auto CUIItem_OnChildNotify = reinterpret_cast<t_CUIItem_OnChildNotify>(kAddr_CUIItem_OnChildNotify);
void __fastcall CUIItem_OnChildNotify_hook(void* pThis, void* /*edx*/, unsigned int nId, unsigned int nCode, unsigned int nParam) {
    if (nId == kInvBtnId) {
        if (nCode == kInvNotifyClick) ToggleBags();    // 0x64 = clicked (0x65 = hover enter/leave); toggle the bag
        return;                                        // consume (vanilla ignores this id)
    }
    CUIItem_OnChildNotify(pThis, nId, nCode, nParam);
}

// CField::OnKey — v95 sym, v83 VA 0x00529968 (ret 8). Search-box typing only; the toggle is the
// Key Config shortcut in damagerankinput.cpp.
static auto CField_OnKey = CField::OnKey;
static void __fastcall CField_OnKey_hook(CField* pThis, void* /*edx*/, unsigned int wParam, int lParam) {
    if (OnFieldKey(wParam, lParam)) return;
    CField_OnKey(pThis, wParam, lParam);
}

// CDraggableItem::OnDropped — v95 sym, v83 VA 0x004EF140 (vtable 0x00AF34D8 [1], ret 0x10, returns int).
// The return value is unused by CWndMan::EndDragDrop's callers for a consumed drop; 1 = handled.
typedef int(__thiscall* t_CDraggableItem_OnDropped)(void*, void*, void*, int, int);
static auto CDraggableItem_OnDropped = reinterpret_cast<t_CDraggableItem_OnDropped>(0x004EF140);
static int __fastcall CDraggableItem_OnDropped_hook(void* pThis, void* /*edx*/, void* pFrom, void* pTo, int rx, int ry) {
    if (HandleDropped(pThis, pFrom, pTo, rx, ry)) return 1;
    return CDraggableItem_OnDropped(pThis, pFrom, pTo, rx, ry);
}

} // namespace BagWindow

void AttachStorageBagMod() {
    using namespace BagWindow;
    ATTACH_HOOK(CField_OnKey, CField_OnKey_hook);
    ATTACH_HOOK(CDraggableItem_OnDropped, CDraggableItem_OnDropped_hook);
    ATTACH_HOOK(CUIItem_OnCreate, CUIItem_OnCreate_hook);
    ATTACH_HOOK(CUIItem_OnChildNotify, CUIItem_OnChildNotify_hook);
}

void StorageBag_Toggle() { BagWindow::ToggleBags(); }

bool StorageBag_TryCloseUI(void* pWnd) {
    using namespace BagWindow;
    CUIBagWindow* w = CUIBagWindow::ms_pInstance;
    if (!pWnd || !w || pWnd != static_cast<CWnd*>(w)) return false;
    w->Destroy();                          // OnDestroy leaves the stack
    return true;
}

void StorageBag_OnLeaveField() {
    using namespace BagWindow;
    s_bLeavingField = true;
    if (CUIBagWindow::ms_pInstance) CUIBagWindow::ms_pInstance->Destroy();   // CWnd::Destroy 0x009E00AF runs OnDestroy synchronously
    s_bLeavingField = false;
    s_bAwaitingSnapshot = false;
    for (int k = 0; k < kKindCount; ++k) { FreeBag(k); s_bAutoOn[k] = false; }   // another character may log in next
}

void StorageBag_HandlePacket(CInPacket* pPacket) { BagWindow::HandlePacket(pPacket); }
