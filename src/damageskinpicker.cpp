#include "pch.h"
#include "hook.h"
#include "debug.h"
#include "damageskin.h"
#include "wvs/iteminfo.h"
#include "wvs/util.h"
#include "wvs/wnd.h"
#include "ztl/ztl.h"

#include <algorithm>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include <windows.h>


// ===========================================================================
// CUIDamageSkinPicker — the "DAMAGE SKIN" browser window.
//
// Opened by double-clicking cash item 5910000. Item is NOT consumed (we
// intercept SendConsumeCashItemUseRequest on the client; server never sees
// the use). Layout mirrors the reference UI:
//
//   +---- DAMAGE SKIN --------------------------[X]+
//   |                   [ PREVIEW ]                |
//   |   +---------- preview area -------------+    |
//   |   |   (selected skin digits rendered)   |    |
//   |   +-------------------------------------+    |
//   |  [ MY DAMAGE SKINS ] [ DAMAGE SKIN SHOP ]    |
//   |   +----------------+  +----------------+     |
//   |   |                |  |                |     |
//   |   | scrollable list|  | scrollable list|     |
//   |   +----------------+  +----------------+     |
//   |   [id][GO] <> [APPLY] [id][GO] <> [PURCHASE] |
//   +-----------------------------------------------+
// ===========================================================================


// Quiet in release builds. Flip to 1 to re-enable damageskin_picker.txt
// trace logging for troubleshooting.
#define DAMAGESKIN_PICKER_TRACE 0
#if DAMAGESKIN_PICKER_TRACE
static FILE* g_pPickerLog = nullptr;
static void PickerLog(const char* fmt, ...) {
    if (!g_pPickerLog) {
        fopen_s(&g_pPickerLog, "damageskin_picker.txt", "a");
        if (g_pPickerLog) {
            SYSTEMTIME st; GetLocalTime(&st);
            fprintf(g_pPickerLog,
                "\n=== Picker session %04d-%02d-%02d %02d:%02d:%02d ===\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        }
    }
    if (!g_pPickerLog) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_pPickerLog, fmt, ap);
    va_end(ap);
    fprintf(g_pPickerLog, "\n");
    fflush(g_pPickerLog);
}
#else
static inline void PickerLog(const char*, ...) {}
#endif


// ---------------------------------------------------------------------------
// v83 addresses + constants
// ---------------------------------------------------------------------------
static constexpr int32_t kCashItem_DamageSkin = 5910000;

static constexpr uintptr_t kAddr_SendConsumeCash    = 0x00A0A63F; // CWvsContext::SendConsumeCashItemUseRequest — v95 sym, v83 VA (ret 10h)
static constexpr uintptr_t kAddr_get_consume_type   = 0x004863D5; // get_consume_cash_item_type — v95 sym, v83 VA (cdecl)

// Layout anchors (430x360 window). Eyeballed from the reference screenshot;
// iterate from here.
static constexpr int kWndW = 430;
static constexpr int kWndH = 360;

// Grid layout for skin thumbnails — 3 rows × 3 columns per side.
// Cell is larger than the raw sprite to give bigger, less-aliased previews;
// adjacent cells still stay clear since positions are spaced ~73x48 apart.
static constexpr int kGridW      = 40;
static constexpr int kGridH      = 44;
static constexpr int kGridPerPage = 9;

// Cell top-left coords per side, row-major (top→bottom, left→right).
// Captured from the origin tool; MY and SHOP measured independently since
// the frame isn't a clean mirror.
static constexpr int kMyGrid[9][2] = {
    { 23, 150}, { 91, 150}, {157, 150},
    { 23, 199}, { 91, 199}, {157, 199},
    { 23, 248}, { 91, 248}, {157, 248},
};
static constexpr int kShopGrid[9][2] = {
    {232, 150}, {299, 150}, {367, 150},
    {232, 199}, {300, 199}, {367, 199},
    {232, 248}, {299, 248}, {367, 248},
};


// get_basic_font — v95 sym, v83 VA 0x0098A707 (cdecl, hidden return + FONT_TYPE). Dotum 12.
static auto get_basic_font =
    reinterpret_cast<IWzFontPtr*(__cdecl*)(IWzFontPtr*, int)>(0x0098A707);

// play_ui_sound — v95 sym, v83 VA 0x00989588 (cdecl). Prepends "Sound/UI.img/" to the leaf name.
static auto play_ui_sound =
    reinterpret_cast<void(__cdecl*)(const wchar_t*)>(0x00989588);

// Engine's native Yes/No dialog. Modal; return code 6 = Yes, anything
// else = No/closed. First arg is a ZXString<char> passed by value (same
// pointer-size as an int since sizeof(ZXString<char>) == 4).
//   CUtilDlg::YesNo(msg, sndName, ZRef<CDialog>*, autoSeparated, tightLine) — v95 sym, v83 VA 0x00992BFD
typedef int(__cdecl* t_CUtilDlg_YesNo)(
    ZXString<char>, const wchar_t*, void*, int, int);
static auto CUtilDlg_YesNo =
    reinterpret_cast<t_CUtilDlg_YesNo>(0x00992BFD);
static constexpr int kYesNo_Yes = 6;

// Engine's native single-button Notice dialog. Same signature as YesNo
// (they share SetUtilDlg under the hood). Used for informational popups
// like "you already own this skin". CUtilDlg::Notice — v95 sym, v83 VA 0x009929DD.
typedef int(__cdecl* t_CUtilDlg_Notice)(
    ZXString<char>, const wchar_t*, void*, int, int);
static auto CUtilDlg_Notice =
    reinterpret_cast<t_CUtilDlg_Notice>(0x009929DD);

// ---------------------------------------------------------------------------
// Engine CUIToolTip bindings
//
// CUIToolTip is a per-UI embedded class. We reserve a raw byte buffer inside
// CUIDamageSkinPicker and call the engine's ctor/dtor/SetString/Clear on it
// directly. The ctor internally creates IWzFont instances via PcCreateObject
// and sets up IWzGr2DLayer slots — so it must run AFTER the engine is
// initialised, never inside DllMain. We lazy-init on first hover.
//
// Critical: ClearToolTip must run BEFORE the dtor; otherwise the layer
// registered with the global Gr2D manager outlives our buffer → crash on
// the next frame render.
// ---------------------------------------------------------------------------
using TT_Ctor      = void*(__thiscall*)(void*);
using TT_Dtor      = void (__thiscall*)(void*);
using TT_SetString = void (__thiscall*)(void*, int, int, const char*);
using TT_Clear     = void (__thiscall*)(void*);

// CUIToolTip ctor / dtor / SetToolTip_String / ClearToolTip — v95 syms, v83 VAs. sizeof(CUIToolTip) = 0x514.
static auto pTT_Ctor      = reinterpret_cast<TT_Ctor>(0x008E49B5);
static auto pTT_Dtor      = reinterpret_cast<TT_Dtor>(0x008E6BA3);
static auto pTT_SetString = reinterpret_cast<TT_SetString>(0x008E6E7D);
static auto pTT_Clear     = reinterpret_cast<TT_Clear>(0x008E6E23);

// Cached dark font for window text. Tries two strategies:
//   1. PcCreateObject(L"GDIFont") + Create(Dotum, 12, black) — may fail
//      silently early in startup.
//   2. Fall back to probing get_basic_font(0..9) for the one whose color is
//      closest to black, and use that.
static IWzFontPtr g_pBlackFont;
static bool       g_bBlackFontTried = false;

static int s_iColorDist(unsigned c) {
    // Distance from black (0x000000) in RGB space — ignore alpha byte.
    int r = (c >> 16) & 0xFF;
    int g = (c >> 8)  & 0xFF;
    int b =  c        & 0xFF;
    return r*r + g*g + b*b;
}

static IWzFont* GetBlackFont() {
    if (g_bBlackFontTried) return g_pBlackFont;
    g_bBlackFontTried = true;

    // Strategy 1: custom font via PcCreateObject.
    try {
        PcCreateObject<IWzFontPtr>(L"GDIFont", g_pBlackFont, nullptr);
        PickerLog("GetBlackFont: PcCreateObject(GDIFont) -> %p",
                  (IWzFont*)g_pBlackFont);
        if (g_pBlackFont) {
            g_pBlackFont->Create(Ztl_bstr_t(L"Dotum"), 12,
                                 0xFF000000, Ztl_variant_t());
            int col = g_pBlackFont->Getcolor();
            PickerLog("  Create returned; Getcolor()=0x%08X", col);
            if ((col & 0xFFFFFF) == 0x000000) return g_pBlackFont;
            // Create "worked" but color isn't black — fall through to probe.
            g_pBlackFont = nullptr;
        }
    } catch (...) {
        PickerLog("GetBlackFont: GDIFont path threw");
        g_pBlackFont = nullptr;
    }

    // Strategy 2: probe the 10 basic fonts, pick darkest.
    int bestIdx = -1;
    int bestDist = INT_MAX;
    for (int i = 0; i < 10; ++i) {
        try {
            IWzFontPtr f;
            get_basic_font(std::addressof(f), i);
            if (!f) continue;
            int col = f->Getcolor();
            int d = s_iColorDist((unsigned)col);
            PickerLog("  basic_font(%d) color=0x%08X dist=%d", i, col, d);
            if (d < bestDist) {
                bestDist = d;
                bestIdx = i;
                g_pBlackFont = f;
            }
        } catch (...) {}
    }
    PickerLog("GetBlackFont: using basic_font(%d)", bestIdx);
    return g_pBlackFont;
}


// ---------------------------------------------------------------------------
// CUIDamageSkinPicker
// ---------------------------------------------------------------------------
class CUIDamageSkinPicker : public CWnd {
public:
    static CRTTI ms_RTTI;
    static CUIDamageSkinPicker* ms_pInstance;

    virtual const CRTTI* GetRTTI() const override { return &ms_RTTI; }
    virtual int IsKindOf(const CRTTI* p) const override {
        return ms_RTTI.IsKindOf(p);
    }

    CUIDamageSkinPicker();
    virtual ~CUIDamageSkinPicker() override;

    virtual void Draw(const RECT* pRect) override;
    virtual void OnMouseButton(unsigned int msg, unsigned int wParam, int rx, int ry) override;
    virtual int  OnMouseMove(int rx, int ry) override;
    virtual void OnDestroy() override;
    virtual void Update() override { InvalidateRect(nullptr); }

    // Returns true if the key was consumed by one of our input boxes.
    bool HandleKey(int vk);
    int  InputFocus() const { return m_nInputFocus; }

    // Rebuilds m_vMyList / m_vShopList from g_vOwnedSkins / g_vShopCatalog.
    // Called from the free function RefreshDamageSkinPicker() after the
    // net layer processes Inventory/Result/Catalog packets.
    void RefreshLists();

private:
    static IWzCanvasPtr LoadSprite(const wchar_t* sPath);
    static IWzCanvasPtr LoadUISprite(const wchar_t* sLeaf);
    static void BlitAt(IWzCanvasPtr pCanvas, IWzCanvasPtr pSprite, int x, int y);

    void LoadSprites();
    void ComputeHitRects();

    void DrawPreview(IWzCanvasPtr pCanvas);
    void DrawSkinGrid(IWzCanvasPtr pCanvas, const int (*cells)[2],
                      const std::vector<int>& list, int scroll, int selected);
    int  HitTestGrid(const int (*cells)[2], int rx, int ry);
    void DrawText(IWzCanvasPtr pCanvas, int x, int y, const wchar_t* text);

    // Sprites
    IWzCanvasPtr m_pBg;
    IWzCanvasPtr m_pBtClose[4];    // normal/pressed/disabled/mouseOver
    IWzCanvasPtr m_pBtApply[4];
    IWzCanvasPtr m_pBtPurchase[4];
    IWzCanvasPtr m_pBtGo[4];
    IWzCanvasPtr m_pPagePrev[3];   // enabled/prev0..2 (animation frames — use frame 0)
    IWzCanvasPtr m_pPageNext[3];
    IWzCanvasPtr m_pPagePrevD;     // disabled
    IWzCanvasPtr m_pPageNextD;
    IWzCanvasPtr m_pPreviewBg[3];  // BackgroundPreview/0, /1, /3
    int m_nPreviewBg;              // 0..2 index into m_pPreviewBg
    IWzCanvasPtr m_pCheck[2];      // check0 (unchecked) / check1 (checked)
    IWzCanvasPtr m_pMesoIcon;      // UI/UIWindow.img/Shop/meso — tooltip price
    bool m_bShowCrit;              // true = preview uses NoCri digits

    // State
    std::vector<int> m_vMyList;    // owned skins, populated from g_vOwnedSkins
    std::vector<int> m_vShopList;  // catalog ids, populated from g_vShopCatalog
    int m_nMyScroll;
    int m_nMySel;                  // index into m_vMyList; -1 = none
    int m_nShopScroll;
    int m_nShopSel;
    int m_nPreviewSide;            // 0=MY, 1=SHOP — drives DrawPreview source
    int m_nShopHoverCell;          // 0..8 of kShopGrid, -1 if not hovered
    int m_nMouseX, m_nMouseY;      // last cursor position (window-local)

    // Hit rects (in window-local coords)
    RECT m_rcClose;
    RECT m_rcApply;
    RECT m_rcPurchase;
    RECT m_rcMyGo;
    RECT m_rcShopGo;
    RECT m_rcMyPrev, m_rcMyNext;
    RECT m_rcShopPrev, m_rcShopNext;
    RECT m_rcMyList;
    RECT m_rcShopList;
    RECT m_rcPreview;
    RECT m_rcPrevBgArrow;
    RECT m_rcNextBgArrow;
    RECT m_rcCheck;          // "show crit preview" checkbox
    RECT m_rcMyCustomCheck;  // MY-side "unit-damage only" filter checkbox
    RECT m_rcShopCustomCheck;// SHOP-side same
    bool m_bMyCustomFilter = false;
    bool m_bShopCustomFilter = false;
    RECT m_rcTitleBar;

    // Page-number display + input (one per side).
    RECT m_rcMyPageText;
    RECT m_rcShopPageText;
    RECT m_rcMyPageInput;
    RECT m_rcShopPageInput;

    std::wstring m_sMyInput;      // typed page-number string
    std::wstring m_sShopInput;
    int m_nInputFocus;            // 0=none, 1=MY, 2=SHOP

    // Interaction
    int m_nPressedBtn;  // 0=none, 1=Apply, 2=Purchase, 3=Close, 4=MyPrev, 5=MyNext, 6=ShopPrev, 7=ShopNext, 8=MyGo, 9=ShopGo
    int m_nHoveredBtn;
    bool m_bDragging;
    int m_nDragAnchorX;
    int m_nDragAnchorY;

    // Engine CUIToolTip, embedded as a raw buffer we manually ctor/dtor.
    // Size 0x600 covers the ctor's writes (highest observed = 0x510) with
    // slack. Aligned to 8 to match typical Win32 class alignment.
    // m_bTtInit tracks whether the ctor succeeded so the dtor only runs
    // when there is something to tear down.
    alignas(8) unsigned char m_ttBuf[0x600];
    bool m_bTtInit;
    int  m_nTtShownCell;           // last shop cell we raised a tooltip for; -1 = none

    // Current window screen position (top-left). CreateWnd sets it; drag
    // accumulates via RelOffset.
    int m_nWndX, m_nWndY;

    void EnsureTooltip();
    void TooltipShow(int screenX, int screenY, const char* text);
    void TooltipHide();
};


CRTTI CUIDamageSkinPicker::ms_RTTI{nullptr};
CUIDamageSkinPicker* CUIDamageSkinPicker::ms_pInstance = nullptr;


IWzCanvasPtr CUIDamageSkinPicker::LoadSprite(const wchar_t* sPath) {
    IWzCanvasPtr pCanvas;
    try {
        pCanvas = get_unknown(get_rm()->GetObjectA(const_cast<wchar_t*>(sPath)));
    } catch (...) {}
    return pCanvas;
}

// Picker art lives in Custom.wz; resman's override merge does not surface new sub-properties under
// UI/UIWindow.img, so it is always read through the Custom/ path.
IWzCanvasPtr CUIDamageSkinPicker::LoadUISprite(const wchar_t* sLeaf) {
    wchar_t sPath[160];
    _snwprintf_s(sPath, _countof(sPath), _TRUNCATE, L"%ls/%ls", kDamageSkinUIUOL, sLeaf);
    return LoadSprite(sPath);
}

void CUIDamageSkinPicker::BlitAt(IWzCanvasPtr pCanvas, IWzCanvasPtr pSprite,
                                 int x, int y)
{
    if (!pCanvas || !pSprite) return;
    pCanvas->CopyEx(x, y, pSprite, CANVAS_ALPHATYPE::CA_REMOVEALPHA,
                    0, 0, 0, 0, 0, 0);
}

void CUIDamageSkinPicker::LoadSprites() {
    m_pBg            = LoadUISprite(L"backgrnd");

    m_pBtClose[0]    = LoadUISprite(L"BtClose/normal/0");
    m_pBtClose[1]    = LoadUISprite(L"BtClose/pressed/0");
    m_pBtClose[2]    = LoadUISprite(L"BtClose/disabled/0");
    m_pBtClose[3]    = LoadUISprite(L"BtClose/mouseOver/0");

    m_pBtApply[0]    = LoadUISprite(L"BtApply/normal/0");
    m_pBtApply[1]    = LoadUISprite(L"BtApply/pressed/0");
    m_pBtApply[2]    = LoadUISprite(L"BtApply/disabled/0");
    m_pBtApply[3]    = LoadUISprite(L"BtApply/mouseOver/0");

    m_pBtPurchase[0] = LoadUISprite(L"BtBuy/normal/0");
    m_pBtPurchase[1] = LoadUISprite(L"BtBuy/pressed/0");
    m_pBtPurchase[2] = LoadUISprite(L"BtBuy/disabled/0");
    m_pBtPurchase[3] = LoadUISprite(L"BtBuy/mouseOver/0");

    m_pBtGo[0]       = LoadUISprite(L"BtGo/normal/0");
    m_pBtGo[1]       = LoadUISprite(L"BtGo/pressed/0");
    m_pBtGo[2]       = LoadUISprite(L"BtGo/disabled/0");
    m_pBtGo[3]       = LoadUISprite(L"BtGo/mouseOver/0");

    m_pPagePrev[0]   = LoadUISprite(L"PageArrow/enabled/prev0");
    m_pPagePrev[1]   = LoadUISprite(L"PageArrow/enabled/prev1");
    m_pPagePrev[2]   = LoadUISprite(L"PageArrow/enabled/prev2");
    m_pPageNext[0]   = LoadUISprite(L"PageArrow/enabled/next0");
    m_pPageNext[1]   = LoadUISprite(L"PageArrow/enabled/next1");
    m_pPageNext[2]   = LoadUISprite(L"PageArrow/enabled/next2");
    m_pPagePrevD     = LoadUISprite(L"PageArrow/disabled/prev");
    m_pPageNextD     = LoadUISprite(L"PageArrow/disabled/next");

    // Preview backgrounds — WZ path is PreviewBackground/0..2.
    m_pPreviewBg[0]  = LoadUISprite(L"PreviewBackground/0");
    m_pPreviewBg[1]  = LoadUISprite(L"PreviewBackground/1");
    m_pPreviewBg[2]  = LoadUISprite(L"PreviewBackground/2");

    // "Show crit preview" checkbox.
    m_pCheck[0]      = LoadUISprite(L"check0");
    m_pCheck[1]      = LoadUISprite(L"check1");

    // Mesos icon used in the shop tooltip.
    m_pMesoIcon      = LoadSprite(L"UI/UIWindow.img/Shop/meso");
    for (int i = 0; i < 3; ++i) {
        UINT w = 0, h = 0;
        if (m_pPreviewBg[i]) {
            m_pPreviewBg[i]->get_width(&w);
            m_pPreviewBg[i]->get_height(&h);
        }
        PickerLog("  PreviewBg[%d] = %p  %ux%u",
                  i, (IWzCanvas*)m_pPreviewBg[i], w, h);
    }

    PickerLog("LoadSprites: bg=%p close=%p apply=%p purchase=%p go=%p prev=%p next=%p",
              (IWzCanvas*)m_pBg, (IWzCanvas*)m_pBtClose[0],
              (IWzCanvas*)m_pBtApply[0], (IWzCanvas*)m_pBtPurchase[0],
              (IWzCanvas*)m_pBtGo[0],
              (IWzCanvas*)m_pPagePrev[0], (IWzCanvas*)m_pPageNext[0]);
}

void CUIDamageSkinPicker::ComputeHitRects() {
    // Title bar (for dragging) — top 28px, minus the close button area.
    m_rcTitleBar  = { 0, 0, kWndW - 20, 28 };

    // Close button: top-right corner inside the title bar.
    m_rcClose     = { 411, 9, 411 + 12, 9 + 12 };

    // Preview area — centered under the "PREVIEW" label.
    m_rcPreview   = { 22, 70, 408, 170 };

    // Preview-background cycler arrows (left / right of the PREVIEW label).
    m_rcPrevBgArrow = { 169, 37, 169 + 13, 37 + 15 };
    m_rcNextBgArrow = { 251, 37, 251 + 13, 37 + 15 };

    // "Show crit preview" checkbox — sits to the left of the preview bg
    // arrows. Sprite size inferred at draw time (default ~14×14).
    m_rcCheck       = { 37, 41, 37 + 14, 41 + 14 };
    // Unit-damage filter checkboxes (user-supplied 12×12 hitboxes).
    m_rcMyCustomCheck   = { 192, 311, 204, 323 };
    m_rcShopCustomCheck = { 404, 311, 416, 323 };

    // MY list — nudged 5px left + 20px up from baseline, sized for 10 rows
    // at kRowH=14 = 140px tall.
    m_rcMyList    = { 17, 150, 210, 290 };
    // SHOP list — nudged 20px up from baseline, same 10-row height.
    m_rcShopList  = { 225, 150, 408, 290 };

    // Bottom area is TWO rows per side:
    //   row A (y≈320): [input] [gap] [prev][next]
    //   row B (y≈340): [GO]    [gap] [APPLY / PURCHASE]
    // MY side (starts at x≈22):
    // Positions captured from the layered origin tool — see origin/index.html.
    m_rcMyPrev    = {  85, 311,  85 + 13, 311 + 15 };
    m_rcMyNext    = { 146, 311, 146 + 13, 311 + 15 };
    m_rcMyGo      = {  10, 330,  10 + 31, 330 + 17 };
    m_rcApply     = { 100, 330, 100 + 44, 330 + 16 };

    m_rcShopPrev  = { 297, 311, 297 + 13, 311 + 15 };
    m_rcShopNext  = { 356, 311, 356 + 13, 311 + 15 };
    m_rcShopGo    = { 219, 330, 219 + 31, 330 + 17 };
    m_rcPurchase  = { 311, 330, 311 + 44, 330 + 16 };

    // Page-number read-out text (between the prev/next arrows). SHOP's
    // origin (329, 312) came from the layered origin tool; MY mirrors it
    // by -208px to match the other SHOP↔MY controls.
    m_rcMyPageText   = { 101, 312, 101 + 24, 312 + 14 };
    m_rcShopPageText = { 309, 312, 309 + 24, 312 + 14 };

    // Input box where the player types a page number; GO jumps to it.
    m_rcMyPageInput   = {  16, 312,  16 + 28, 312 + 14 };
    m_rcShopPageInput = { 224, 312, 224 + 28, 312 + 14 };
}

CUIDamageSkinPicker::CUIDamageSkinPicker()
    : m_nPreviewBg(0), m_bShowCrit(false),
      m_nMyScroll(0), m_nMySel(-1),
      m_nShopScroll(0), m_nShopSel(-1),
      m_nPreviewSide(0),
      m_nShopHoverCell(-1), m_nMouseX(0), m_nMouseY(0),
      m_nInputFocus(0),
      m_nPressedBtn(0), m_nHoveredBtn(0),
      m_bDragging(false), m_nDragAnchorX(0), m_nDragAnchorY(0),
      m_bTtInit(false), m_nTtShownCell(-1),
      m_nWndX(0), m_nWndY(0)
{
    ms_pInstance = this;
    // Zero the tooltip buffer — matches what the engine's compilers emit
    // when embedding; ensures any unused slots start at known state.
    memset(m_ttBuf, 0, sizeof(m_ttBuf));

    int x = (get_screen_width()  - kWndW) / 2;
    int y = (get_screen_height() - kWndH) / 2;
    m_nWndX = x; m_nWndY = y;
    CWnd::CreateWnd(this, x, y, kWndW, kWndH, 10, 1, nullptr, 1);
    PickerLog("ctor: window at (%d,%d) %dx%d", x, y, kWndW, kWndH);

    play_ui_sound(L"MenuUp");

    LoadSprites();
    ComputeHitRects();

    LoadDamageSkin();
    RefreshLists();

    // Pre-select the currently-active skin (if any) in MY list.
    if (g_nActiveSkin != 0) {
        for (size_t i = 0; i < m_vMyList.size(); ++i) {
            if (m_vMyList[i] == g_nActiveSkin) {
                m_nMySel = (int)i;
                break;
            }
        }
    }

    PickerLog("ctor: MY=%zu SHOP=%zu active=%d mySel=%d",
              m_vMyList.size(), m_vShopList.size(),
              g_nActiveSkin, m_nMySel);
}

void CUIDamageSkinPicker::RefreshLists() {
    // Lambda: if filter is on, only accept skins whose DamageSkinProp
    // carries bHasCustom (i.e. NoCustom with both NoRed0 and NoCri0).
    auto keep = [](int id, bool filterUnitOnly) {
        if (!filterUnitOnly) return true;
        auto it = g_mDamageSkinProp.find(id);
        return it != g_mDamageSkinProp.end() && it->second.bHasCustom;
    };

    // MY = server-confirmed owned skins only. Default (id 0) NOT listed.
    m_vMyList.clear();
    for (int id : g_vOwnedSkins) {
        if (id == 0) continue;
        if (!keep(id, m_bMyCustomFilter)) continue;
        m_vMyList.push_back(id);
    }

    // SHOP = server-sent catalog, filtered to ids whose WZ data is loaded
    // on this client, then optionally filtered to unit-only.
    m_vShopList.clear();
    for (const auto& e : g_vShopCatalog) {
        if (e.nID == 0) continue;
        if (g_mDamageSkinProp.find(e.nID) == g_mDamageSkinProp.end()) continue;
        if (!keep(e.nID, m_bShopCustomFilter)) continue;
        m_vShopList.push_back(e.nID);
    }
    // Fallback to every loaded WZ skin only if the server never sent a
    // catalog at all (offline / WZ-only testing).
    if (m_vShopList.empty() && g_vShopCatalog.empty()) {
        for (int id : g_vSkinIds) {
            if (id == 0) continue;
            if (!keep(id, m_bShopCustomFilter)) continue;
            m_vShopList.push_back(id);
        }
    }

    // Clamp selections + scroll so a refresh after a purchase doesn't
    // leave stale indices pointing past the new list end.
    if (m_nMySel   >= (int)m_vMyList.size())   m_nMySel   = -1;
    if (m_nShopSel >= (int)m_vShopList.size()) m_nShopSel = -1;
    if (m_nMyScroll   >= (int)m_vMyList.size())   m_nMyScroll   = 0;
    if (m_nShopScroll >= (int)m_vShopList.size()) m_nShopScroll = 0;

    PickerLog("RefreshLists: catalog=%zu props=%zu owned=%zu  ->  MY=%zu SHOP=%zu",
              g_vShopCatalog.size(), g_mDamageSkinProp.size(), g_vOwnedSkins.size(),
              m_vMyList.size(), m_vShopList.size());

    InvalidateRect(nullptr);
}

// Free function exposed via damageskin.h so the net layer in damageskin.cpp
// can poke the picker without knowing its class layout.
void RefreshDamageSkinPicker() {
    if (CUIDamageSkinPicker::ms_pInstance) {
        CUIDamageSkinPicker::ms_pInstance->RefreshLists();
    }
}

CUIDamageSkinPicker::~CUIDamageSkinPicker() {
    // CRITICAL ordering for the engine tooltip:
    //   1. ClearToolTip — removes the IWzGr2DLayer this tooltip registered
    //      with the global Gr2D manager. Without this the manager keeps
    //      a dangling pointer into m_ttBuf and crashes on the next frame.
    //   2. CUIToolTip dtor — frees COM refs (IWzFont, sub-layers, etc.).
    // Only run the dtor when ctor actually succeeded.
    if (m_bTtInit) {
        try { pTT_Clear(m_ttBuf); } catch (...) {}
        try { pTT_Dtor(m_ttBuf);  } catch (...) {}
        m_bTtInit = false;
    }
    if (ms_pInstance == this) ms_pInstance = nullptr;
    PickerLog("dtor");
}

void CUIDamageSkinPicker::OnDestroy() {
    // OnDestroy fires before ~. Hide the tooltip here too so that any frame
    // rendered between OnDestroy and ~ doesn't paint a stale layer.
    if (m_bTtInit) {
        try { pTT_Clear(m_ttBuf); } catch (...) {}
    }
    if (ms_pInstance == this) ms_pInstance = nullptr;
    CWnd::OnDestroy();
}

void CUIDamageSkinPicker::DrawText(IWzCanvasPtr pCanvas, int x, int y,
                                   const wchar_t* text)
{
    try {
        IWzFont* pFont = GetBlackFont();
        IWzFontPtr pBasic;  // fallback holder — must outlive the call
        if (!pFont) {
            get_basic_font(std::addressof(pBasic), 0);
            pFont = pBasic;
        }
        if (pFont) {
            pCanvas->DrawTextA(x, y, Ztl_bstr_t(text), pFont,
                               Ztl_variant_t(), Ztl_variant_t());
        }
    } catch (...) {
        PickerLog("DrawText threw at (%d,%d)", x, y);
    }
}

void CUIDamageSkinPicker::DrawPreview(IWzCanvasPtr pCanvas) {
    // Preview follows whichever panel was last clicked.
    int skinId = 0;
    if (m_nPreviewSide == 1 &&
        m_nShopSel >= 0 && m_nShopSel < (int)m_vShopList.size()) {
        skinId = m_vShopList[m_nShopSel];
    } else if (m_nMySel >= 0 && m_nMySel < (int)m_vMyList.size()) {
        skinId = m_vMyList[m_nMySel];
    } else {
        skinId = g_nActiveSkin;
    }
    if (skinId == 0) return;
    auto it = g_mDamageSkinProp.find(skinId);
    if (it == g_mDamageSkinProp.end()) return;

    // Whichever side drives the preview also picks the filter state —
    // MY panel's filter when previewing MY, SHOP's when previewing SHOP.
    bool unitOnly = (m_nPreviewSide == 1) ? m_bShopCustomFilter
                                          : m_bMyCustomFilter;

    // Base digit sheets (crit-toggle respected).
    IWzPropertyPtr pSheet = m_bShowCrit ? it->second.pNoCri0
                                        : it->second.pNoRed0;
    if (!pSheet) pSheet = it->second.pNoRed0;
    if (!pSheet) return;

    auto fetch = [](IWzPropertyPtr p, const wchar_t* name) -> IWzCanvasPtr {
        if (!p) return nullptr;
        try {
            Ztl_variant_t v;
            if (SUCCEEDED(p->get_item(const_cast<wchar_t*>(name), &v))
                && v.vt == VT_UNKNOWN) {
                return IWzCanvasPtr(v.GetUnknown(false, false));
            }
        } catch (...) {}
        return nullptr;
    };

    // Collect the glyph run. When filter is on AND the skin has NoCustom,
    // we render "1.8K" instead of "12345" — same centre position.
    struct Glyph { IWzCanvasPtr pC; int w, h; };
    std::vector<Glyph> glyphs;
    int totalW = 0, maxH = 0;

    if (unitOnly && it->second.bHasCustom) {
        IWzPropertyPtr pUnit = m_bShowCrit ? it->second.pNoCustomCri0
                                           : it->second.pNoCustomRed0;
        if (!pUnit) pUnit = it->second.pNoCustomRed0;

        // "1.8K" sequence:
        //   regular '1', NoCustom '.' (child "0"),
        //   regular '8', NoCustom 'K' (child "1").
        struct Piece { IWzPropertyPtr src; const wchar_t* name; };
        Piece pieces[] = {
            { pSheet, L"1" },
            { pUnit,  L"0" },
            { pSheet, L"8" },
            { pUnit,  L"1" },
        };
        for (auto& p : pieces) {
            IWzCanvasPtr pC = fetch(p.src, p.name);
            if (!pC) continue;
            UINT w = 0, h = 0;
            pC->get_width(&w);
            pC->get_height(&h);
            glyphs.push_back({ pC, (int)w, (int)h });
            totalW += (int)w + 1;
            if ((int)h > maxH) maxH = (int)h;
        }
    } else {
        // Regular "12345" run.
        for (wchar_t d : std::wstring(L"12345")) {
            wchar_t name[2] = { d, 0 };
            IWzCanvasPtr pC = fetch(pSheet, name);
            if (!pC) continue;
            UINT w = 0, h = 0;
            pC->get_width(&w);
            pC->get_height(&h);
            glyphs.push_back({ pC, (int)w, (int)h });
            totalW += (int)w + 1;
            if ((int)h > maxH) maxH = (int)h;
        }
    }

    if (glyphs.empty()) return;

    int cx = (m_rcPreview.left + m_rcPreview.right) / 2;
    int cy = (m_rcPreview.top + m_rcPreview.bottom) / 2;
    int px = cx - totalW / 2;
    int py = cy - maxH / 2 - 31;  // same anchor as the regular preview

    for (auto& g : glyphs) {
        BlitAt(pCanvas, g.pC, px, py);
        px += g.w + 1;
    }
}

// ---------------------------------------------------------------------------
// Engine tooltip wiring
// ---------------------------------------------------------------------------
void CUIDamageSkinPicker::EnsureTooltip() {
    if (m_bTtInit) return;
    try {
        pTT_Ctor(m_ttBuf);
        m_bTtInit = true;
        PickerLog("EnsureTooltip: CUIToolTip ctor ok");
    } catch (...) {
        PickerLog("EnsureTooltip: ctor threw — tooltip disabled");
        m_bTtInit = false;
    }
}

void CUIDamageSkinPicker::TooltipShow(int screenX, int screenY, const char* text) {
    EnsureTooltip();
    if (!m_bTtInit) return;
    try {
        // Prepend leading spaces so the engine's auto-sized tooltip has
        // room for the meso icon we overlay afterwards. 6 spaces ≈ 28px
        // at Dotum 12, enough for the 20x18-ish Shop/meso sprite.
        char padded[96];
        _snprintf_s(padded, sizeof(padded), _TRUNCATE, "      %s", text);
        pTT_SetString(m_ttBuf, screenX, screenY, padded);

        // Overlay the meso icon onto the tooltip's own layer canvas so it
        // sits ABOVE the picker layer (which the engine tooltip already
        // is). Layer pointer lives at m_ttBuf + 0x10 (observed in IDA —
        // `this[4]` in the decompiled MakeLayer).
        if (m_pMesoIcon) {
            IWzGr2DLayer* pLayer =
                *reinterpret_cast<IWzGr2DLayer**>(m_ttBuf + 0x10);
            if (pLayer) {
                IWzCanvasPtr pTTCanvas = pLayer->Getcanvas(Ztl_variant_t());
                if (pTTCanvas) {
                    // Left pad ≈ 3px, vertical center (Dotum 12 baseline
                    // puts text at y~1..13, icon roughly the same height).
                    pTTCanvas->CopyEx(3, 1, m_pMesoIcon,
                                      CANVAS_ALPHATYPE::CA_REMOVEALPHA,
                                      0, 0, 0, 0, 0, 0, Ztl_variant_t());
                }
            }
        }
    } catch (...) {
        PickerLog("TooltipShow: threw");
    }
}

void CUIDamageSkinPicker::TooltipHide() {
    if (!m_bTtInit) return;
    try {
        pTT_Clear(m_ttBuf);
    } catch (...) {
        PickerLog("TooltipHide: ClearToolTip threw");
    }
    m_nTtShownCell = -1;
}

// Render up to 9 skin thumbnails at the given cell positions. Each
// thumbnail is the skin's NoRed0/"8" digit — loaded from
//   Custom/BasicEff/BasicEff.img/damageSkin/<id>/NoRed0/8
// which is g_mDamageSkinProp[id].pNoRed0->get_item(L"8").
void CUIDamageSkinPicker::DrawSkinGrid(IWzCanvasPtr pCanvas,
                                       const int (*cells)[2],
                                       const std::vector<int>& list,
                                       int scroll, int selected)
{
    for (int i = 0; i < kGridPerPage; ++i) {
        int idx = scroll + i;
        if (idx < 0 || idx >= (int)list.size()) break;
        int x = cells[i][0];
        int y = cells[i][1];

        int skinId = list[idx];
        auto it = g_mDamageSkinProp.find(skinId);
        if (it == g_mDamageSkinProp.end() || !it->second.pNoRed0) continue;
        try {
            Ztl_variant_t v;
            if (FAILED(it->second.pNoRed0->get_item(
                    const_cast<wchar_t*>(L"8"), &v))) continue;
            if (v.vt != VT_UNKNOWN) continue;
            IWzCanvasPtr pThumb(v.GetUnknown(false, false));
            if (!pThumb) continue;

            UINT tw = 0, th = 0;
            pThumb->get_width(&tw);
            pThumb->get_height(&th);
            if (tw == 0 || th == 0) continue;

            // Scale to fit inside the cell (with a small pad), preserving
            // aspect. CopyEx supports scaling via differing dst/src sizes.
            constexpr int kPad = 2;
            int usableW = kGridW - kPad;
            int usableH = kGridH - kPad;
            double sx = (double)usableW / (double)tw;
            double sy = (double)usableH / (double)th;
            double s  = (sx < sy) ? sx : sy;
            if (s > 1.0) s = 1.0;
            int drawW = (int)((double)tw * s + 0.5);
            int drawH = (int)((double)th * s + 0.5);
            int dstX  = x + (kGridW - drawW) / 2;
            int dstY  = y + (kGridH - drawH) / 2;

            // Selection halo — fixed-size semi-transparent gray block
            // centered on the icon, drawn behind it.
            if (idx == selected) {
                constexpr int      kSelectionW   = 39;
                constexpr int      kSelectionH   = 37;
                constexpr unsigned kSelectionCol = 0x804499E8; // #4499e8 @ 50% alpha
                int hx = dstX + drawW / 2 - kSelectionW / 2;
                int hy = dstY + drawH / 2 - kSelectionH / 2;
                pCanvas->DrawRectangle(hx, hy, kSelectionW, kSelectionH,
                                       kSelectionCol);
            }

            pCanvas->CopyEx(dstX, dstY, pThumb, CANVAS_ALPHATYPE::CA_REMOVEALPHA,
                            drawW, drawH, 0, 0, (int)tw, (int)th,
                            Ztl_variant_t());
        } catch (...) {}
    }
}

int CUIDamageSkinPicker::HitTestGrid(const int (*cells)[2], int rx, int ry) {
    for (int i = 0; i < kGridPerPage; ++i) {
        RECT r{ cells[i][0], cells[i][1],
                cells[i][0] + kGridW, cells[i][1] + kGridH };
        POINT pt{ rx, ry };
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

void CUIDamageSkinPicker::Draw(const RECT* pRect) {
    if (this != ms_pInstance) return;
    CWnd::Draw(pRect);
    IWzCanvasPtr pCanvas = GetCanvas();
    if (!pCanvas) return;

    static int s_nDrawCalls = 0;
    if (s_nDrawCalls < 3) {
        ++s_nDrawCalls;
        PickerLog("Draw #%d: begin (canvas=%p bg=%p)",
                  s_nDrawCalls, (IWzCanvas*)pCanvas, (IWzCanvas*)m_pBg);
    }

    // Window background.
    BlitAt(pCanvas, m_pBg, 0, 0);

    // Preview pane: background image (one of 3) pinned at fixed top-left.
    if (m_pPreviewBg[m_nPreviewBg]) {
        BlitAt(pCanvas, m_pPreviewBg[m_nPreviewBg], 13, 56);
    }

    // Preview digits (on top of the preview background).
    try { DrawPreview(pCanvas); }
    catch (...) { PickerLog("Draw: DrawPreview threw"); }

    // "Show crit preview" checkbox.
    BlitAt(pCanvas, m_bShowCrit ? m_pCheck[1] : m_pCheck[0],
           m_rcCheck.left, m_rcCheck.top);

    // "Unit-damage only" filter checkboxes — one per panel.
    BlitAt(pCanvas, m_bMyCustomFilter ? m_pCheck[1] : m_pCheck[0],
           m_rcMyCustomCheck.left,   m_rcMyCustomCheck.top);
    BlitAt(pCanvas, m_bShopCustomFilter ? m_pCheck[1] : m_pCheck[0],
           m_rcShopCustomCheck.left, m_rcShopCustomCheck.top);

    // Preview-background cycler arrows — 3-frame state: 0=normal,
    // 1=mouseOver, 2=pressed (matches WZ enabled/prevN + nextN convention).
    auto pickArrow = [](IWzCanvasPtr* frames, bool pressed, bool hover) {
        if (pressed && frames[2]) return frames[2];
        if (hover   && frames[1]) return frames[1];
        return frames[0];
    };
    BlitAt(pCanvas,
           pickArrow(m_pPagePrev, m_nPressedBtn == 10, m_nHoveredBtn == 10),
           m_rcPrevBgArrow.left, m_rcPrevBgArrow.top);
    BlitAt(pCanvas,
           pickArrow(m_pPageNext, m_nPressedBtn == 11, m_nHoveredBtn == 11),
           m_rcNextBgArrow.left, m_rcNextBgArrow.top);

    // MY and SHOP grids — 3x3 of skin thumbnails.
    DrawSkinGrid(pCanvas, kMyGrid,   m_vMyList,   m_nMyScroll,   m_nMySel);
    DrawSkinGrid(pCanvas, kShopGrid, m_vShopList, m_nShopScroll, m_nShopSel);

    // Page arrows (enabled frame 0; grey the disabled state at edges).
    bool myAtStart = m_nMyScroll <= 0;
    bool myAtEnd   = m_nMyScroll + kGridPerPage >= (int)m_vMyList.size();
    BlitAt(pCanvas, myAtStart ? m_pPagePrevD : m_pPagePrev[0],
           m_rcMyPrev.left, m_rcMyPrev.top);
    BlitAt(pCanvas, myAtEnd   ? m_pPageNextD : m_pPageNext[0],
           m_rcMyNext.left, m_rcMyNext.top);

    bool shopAtStart = m_nShopScroll <= 0;
    bool shopAtEnd   = m_nShopScroll + kGridPerPage >= (int)m_vShopList.size();
    BlitAt(pCanvas, shopAtStart ? m_pPagePrevD : m_pPagePrev[0],
           m_rcShopPrev.left, m_rcShopPrev.top);
    BlitAt(pCanvas, shopAtEnd   ? m_pPageNextD : m_pPageNext[0],
           m_rcShopNext.left, m_rcShopNext.top);

    // Buttons — pressed/hover/normal priority.
    auto pickBt = [](IWzCanvasPtr n, IWzCanvasPtr p, IWzCanvasPtr m,
                     bool pressed, bool hover) -> IWzCanvasPtr {
        if (pressed && p) return p;
        if (hover && m)   return m;
        return n;
    };

    BlitAt(pCanvas, pickBt(m_pBtGo[0], m_pBtGo[1], m_pBtGo[3],
                           m_nPressedBtn == 8, m_nHoveredBtn == 8),
           m_rcMyGo.left, m_rcMyGo.top);
    BlitAt(pCanvas, pickBt(m_pBtGo[0], m_pBtGo[1], m_pBtGo[3],
                           m_nPressedBtn == 9, m_nHoveredBtn == 9),
           m_rcShopGo.left, m_rcShopGo.top);

    BlitAt(pCanvas, pickBt(m_pBtApply[0], m_pBtApply[1], m_pBtApply[3],
                           m_nPressedBtn == 1, m_nHoveredBtn == 1),
           m_rcApply.left, m_rcApply.top);
    BlitAt(pCanvas, pickBt(m_pBtPurchase[0], m_pBtPurchase[1], m_pBtPurchase[3],
                           m_nPressedBtn == 2, m_nHoveredBtn == 2),
           m_rcPurchase.left, m_rcPurchase.top);

    BlitAt(pCanvas, pickBt(m_pBtClose[0], m_pBtClose[1], m_pBtClose[3],
                           m_nPressedBtn == 3, m_nHoveredBtn == 3),
           m_rcClose.left, m_rcClose.top);

    // Page-number read-out, centered between the arrows. White text with a
    // 3×3 black "convolution" shadow: draw black at the 8 surrounding
    // offsets, then white once at the center. Creates a thick dark outline.
    auto drawPage = [&](const RECT& rcPrev, const RECT& rcNext,
                        int scroll, int total) {
        int totalPages = (total + kGridPerPage - 1) / kGridPerPage;
        if (totalPages < 1) totalPages = 1;
        int curPage = (scroll / kGridPerPage) + 1;
        wchar_t buf[24];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d/%d", curPage, totalPages);

        IWzFontPtr pShadow, pWhite;
        get_basic_font(std::addressof(pShadow), 2);  // 0x404040 dark gray — reads like ~45% black
        get_basic_font(std::addressof(pWhite),  0);  // white

        int textW = 0;
        if (pShadow) textW = (int)pShadow->CalcTextWidth(Ztl_bstr_t(buf), Ztl_variant_t());
        int midX = ((rcPrev.left + rcPrev.right) + (rcNext.left + rcNext.right)) / 4;
        int midY = (rcPrev.top + rcPrev.bottom) / 2;
        int x = midX - textW / 2;
        int y = midY - 7;

        if (pShadow) {
            // 3×3 convolution (8 offsets) in solid black — 1px outline,
            // thin enough the digits remain readable.
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    if (dx == 0 && dy == 0) continue;
                    pCanvas->DrawTextA(x + dx, y + dy, Ztl_bstr_t(buf),
                                       pShadow, Ztl_variant_t(), Ztl_variant_t());
                }
            }
        }
        if (pWhite) {
            pCanvas->DrawTextA(x, y, Ztl_bstr_t(buf), pWhite,
                               Ztl_variant_t(), Ztl_variant_t());
        }
    };
    drawPage(m_rcMyPrev,   m_rcMyNext,   m_nMyScroll,   (int)m_vMyList.size());
    drawPage(m_rcShopPrev, m_rcShopNext, m_nShopScroll, (int)m_vShopList.size());

    // Input-box contents + a blinking caret when the box has focus.
    // Caret toggles every 500ms via GetTickCount (engine redraws ensure
    // the tick is re-checked each frame so the line animates).
    bool caretOn = (GetTickCount() / 500u) & 1u;
    auto drawInput = [&](const RECT& r, const std::wstring& txt, bool focused) {
        constexpr int kTextLeftPad = 2;
        if (!txt.empty()) {
            DrawText(pCanvas, r.left + kTextLeftPad, r.top + 1, txt.c_str());
        }
        if (focused && caretOn) {
            // Measure typed text so the caret sits just past the last digit.
            int textW = 0;
            if (!txt.empty()) {
                IWzFont* pFont = GetBlackFont();
                if (pFont) {
                    textW = (int)pFont->CalcTextWidth(Ztl_bstr_t(txt.c_str()),
                                                     Ztl_variant_t());
                }
            }
            int cx = r.left + kTextLeftPad + textW;
            int cy = r.top + 2;                      // nudged ~half a pixel lower
            int ch = (r.bottom - r.top) - 3;         // keep bottom aligned
            pCanvas->DrawRectangle(cx, cy, 1, ch, 0xFF000000);
        }
    };
    drawInput(m_rcMyPageInput,   m_sMyInput,   m_nInputFocus == 1);
    drawInput(m_rcShopPageInput, m_sShopInput, m_nInputFocus == 2);
    // Shop tooltip is now rendered by the engine's CUIToolTip layer —
    // see OnMouseMove/TooltipShow for lifecycle.
}

int CUIDamageSkinPicker::OnMouseMove(int rx, int ry) {
    POINT pt{ rx, ry };

    // Drag the window by the title bar.
    if (m_bDragging && m_pLayer) {
        int dx = rx - m_nDragAnchorX;
        int dy = ry - m_nDragAnchorY;
        if ((dx || dy)) {
            m_pLayer->RelOffset(dx, dy, Ztl_variant_t(), Ztl_variant_t());
            m_nWndX += dx;
            m_nWndY += dy;
            // Hide the tooltip while dragging so it doesn't trail the cursor.
            TooltipHide();
        }
        return 1;
    }

    int nNow = 0;
    if      (PtInRect(&m_rcClose,       pt)) nNow = 3;
    else if (PtInRect(&m_rcApply,       pt)) nNow = 1;
    else if (PtInRect(&m_rcPurchase,    pt)) nNow = 2;
    else if (PtInRect(&m_rcMyPrev,      pt)) nNow = 4;
    else if (PtInRect(&m_rcMyNext,      pt)) nNow = 5;
    else if (PtInRect(&m_rcShopPrev,    pt)) nNow = 6;
    else if (PtInRect(&m_rcShopNext,    pt)) nNow = 7;
    else if (PtInRect(&m_rcMyGo,        pt)) nNow = 8;
    else if (PtInRect(&m_rcShopGo,      pt)) nNow = 9;
    else if (PtInRect(&m_rcPrevBgArrow, pt)) nNow = 10;
    else if (PtInRect(&m_rcNextBgArrow, pt)) nNow = 11;

    // Disabled arrows (at page edge) neither hover-visual nor sound.
    bool disabled =
        (nNow == 4 && m_nMyScroll   <= 0) ||
        (nNow == 5 && m_nMyScroll   + kGridPerPage >= (int)m_vMyList.size()) ||
        (nNow == 6 && m_nShopScroll <= 0) ||
        (nNow == 7 && m_nShopScroll + kGridPerPage >= (int)m_vShopList.size());
    if (disabled) nNow = 0;

    if (nNow != m_nHoveredBtn) {
        if (nNow != 0) play_ui_sound(L"BtMouseOver");
        m_nHoveredBtn = nNow;
    }

    // Track SHOP grid hover and fire the engine's real CUIToolTip.
    m_nMouseX = rx;
    m_nMouseY = ry;
    int shopCell = HitTestGrid(kShopGrid, rx, ry);
    m_nShopHoverCell = shopCell;

    if (shopCell >= 0) {
        int idx = m_nShopScroll + shopCell;
        if (idx >= 0 && idx < (int)m_vShopList.size()) {
            // Only re-assert the tooltip when the cell actually changes —
            // SetToolTip_String re-parses the string and rebuilds the
            // layer, so calling it every pixel of mouse motion thrashes.
            if (idx != m_nTtShownCell) {
                // Look up the real catalog price; fall back to the
                // placeholder if the server hasn't pushed a catalog yet.
                int skinId = m_vShopList[idx];
                long long price = 10000000LL;
                for (const auto& e : g_vShopCatalog) {
                    if (e.nID == skinId) { price = e.llPrice; break; }
                }
                // Format with thousand separators, ANSI (SetToolTip_String
                // takes char*).
                char formatted[32];
                char digits[24];
                int n = 0;
                long long tmp = price <= 0 ? 0 : price;
                if (tmp == 0) { digits[n++] = '0'; }
                while (tmp > 0 && n < 23) {
                    digits[n++] = (char)('0' + (tmp % 10));
                    tmp /= 10;
                }
                // Write digits left-to-right and insert a comma AFTER a
                // digit when the number of digits still to come is a
                // positive multiple of 3. That's "every 3 from the right".
                int w = 0;
                for (int k = 0; k < n && w < 30; ++k) {
                    formatted[w++] = digits[n - 1 - k];
                    int left = n - 1 - k;
                    if (left > 0 && left % 3 == 0 && w < 30) formatted[w++] = ',';
                }
                formatted[w] = 0;

                int sx = m_nWndX + rx + 16;
                int sy = m_nWndY + ry + 20;
                TooltipShow(sx, sy, formatted);
                m_nTtShownCell = idx;
            }
            return 0;
        }
    }
    TooltipHide();
    return 0;
}

void CUIDamageSkinPicker::OnMouseButton(unsigned int msg, unsigned int /*wParam*/,
                                        int rx, int ry)
{
    POINT pt{ rx, ry };

    if (msg == WM_LBUTTONDOWN) {
        // For pageable arrows, `enabled` gates both the click sound and
        // the pressed-state visual — disabled arrows are inert.
        auto press = [this](int id, bool enabled = true) {
            if (!enabled) return;
            m_nPressedBtn = id;
            play_ui_sound(L"BtMouseClick");
            InvalidateRect(nullptr);
        };
        bool myPrevOn   = m_nMyScroll   > 0;
        bool myNextOn   = m_nMyScroll   + kGridPerPage < (int)m_vMyList.size();
        bool shopPrevOn = m_nShopScroll > 0;
        bool shopNextOn = m_nShopScroll + kGridPerPage < (int)m_vShopList.size();

        if (PtInRect(&m_rcCheck, pt)) {
            m_bShowCrit = !m_bShowCrit;
            play_ui_sound(L"BtMouseClick");
            InvalidateRect(nullptr);
            return;
        }
        if (PtInRect(&m_rcMyCustomCheck, pt)) {
            m_bMyCustomFilter = !m_bMyCustomFilter;
            play_ui_sound(L"BtMouseClick");
            // Reset scroll so the shorter filtered list starts at the top.
            m_nMyScroll = 0;
            m_nMySel    = -1;
            RefreshLists();
            return;
        }
        if (PtInRect(&m_rcShopCustomCheck, pt)) {
            m_bShopCustomFilter = !m_bShopCustomFilter;
            play_ui_sound(L"BtMouseClick");
            m_nShopScroll = 0;
            m_nShopSel    = -1;
            RefreshLists();
            return;
        }
        if (PtInRect(&m_rcPrevBgArrow, pt)) { press(10); return; }
        if (PtInRect(&m_rcNextBgArrow, pt)) { press(11); return; }
        if (PtInRect(&m_rcClose,       pt)) { press(3);  return; }
        if (PtInRect(&m_rcApply,       pt)) { press(1);  return; }
        if (PtInRect(&m_rcPurchase,    pt)) { press(2);  return; }
        if (PtInRect(&m_rcMyPrev,      pt)) { press(4, myPrevOn);   return; }
        if (PtInRect(&m_rcMyNext,      pt)) { press(5, myNextOn);   return; }
        if (PtInRect(&m_rcShopPrev,    pt)) { press(6, shopPrevOn); return; }
        if (PtInRect(&m_rcShopNext,    pt)) { press(7, shopNextOn); return; }
        if (PtInRect(&m_rcMyGo,        pt)) { press(8);  return; }
        if (PtInRect(&m_rcShopGo,      pt)) { press(9);  return; }

        // Page-input boxes: click to focus. Clicking outside clears focus.
        if (PtInRect(&m_rcMyPageInput,   pt)) {
            m_nInputFocus = 1; InvalidateRect(nullptr); return;
        }
        if (PtInRect(&m_rcShopPageInput, pt)) {
            m_nInputFocus = 2; InvalidateRect(nullptr); return;
        }
        m_nInputFocus = 0;

        // Grid-cell selection (MY + SHOP). Track which side was last
        // clicked so the preview follows whichever panel the user is
        // browsing, not always MY.
        int cell = HitTestGrid(kMyGrid, rx, ry);
        if (cell >= 0) {
            int idx = m_nMyScroll + cell;
            if (idx >= 0 && idx < (int)m_vMyList.size()) {
                m_nMySel = idx;
                m_nPreviewSide = 0;
            }
            InvalidateRect(nullptr);
            return;
        }
        cell = HitTestGrid(kShopGrid, rx, ry);
        if (cell >= 0) {
            int idx = m_nShopScroll + cell;
            if (idx >= 0 && idx < (int)m_vShopList.size()) {
                m_nShopSel = idx;
                m_nPreviewSide = 1;
            }
            InvalidateRect(nullptr);
            return;
        }

        // Title bar drag.
        if (PtInRect(&m_rcTitleBar, pt)) {
            m_bDragging = true;
            m_nDragAnchorX = rx;
            m_nDragAnchorY = ry;
            return;
        }
    }
    else if (msg == WM_LBUTTONUP) {
        m_bDragging = false;
        int pressed = m_nPressedBtn;
        m_nPressedBtn = 0;

        if (pressed == 3 && PtInRect(&m_rcClose, pt)) {
            PickerLog("Close clicked — destroying window");
            play_ui_sound(L"MenuDown");
            Destroy();
            return;
        }
        if (pressed == 1 && PtInRect(&m_rcApply, pt)) {
            // Send to server; server echoes success via DAMAGE_SKIN_RESULT
            // which updates g_nActiveSkin and broadcasts to the map.
            if (m_nMySel >= 0 && m_nMySel < (int)m_vMyList.size()) {
                int id = m_vMyList[m_nMySel];
                Send_DamageSkinApply(id);
                play_ui_sound(L"anvil");
                PickerLog("Apply clicked — requested skin=%d", id);
            }
            InvalidateRect(nullptr);
            return;
        }
        if (pressed == 2 && PtInRect(&m_rcPurchase, pt)) {
            // Confirm with the engine's native Yes/No dialog before
            // sending the purchase packet. Server re-validates anyway
            // (price/balance/ownership), but a UI confirm prevents
            // accidental double-clicks from burning mesos.
            if (m_nShopSel >= 0 && m_nShopSel < (int)m_vShopList.size()) {
                int id = m_vShopList[m_nShopSel];

                // Already-owned short-circuit: don't even send the
                // purchase. Server would reject with ok=0 but the user
                // gets no feedback. Pop an informational notice instead.
                bool alreadyOwned = false;
                for (int oid : g_vOwnedSkins) {
                    if (oid == id) { alreadyOwned = true; break; }
                }
                if (alreadyOwned) {
                    TooltipHide();
                    play_ui_sound(L"BtMouseClick");
                    ZXString<char> zmsg("You already have this damage skin.");
                    try { CUtilDlg_Notice(zmsg, nullptr, nullptr, 0, 0); }
                    catch (...) { PickerLog("Notice threw"); }
                    PickerLog("Purchase blocked — already owned skin=%d", id);
                    return;
                }

                long long price = 10000000LL;
                for (const auto& e : g_vShopCatalog) {
                    if (e.nID == id) { price = e.llPrice; break; }
                }

                // Format "10,000,000" with thousand separators.
                char priceStr[32];
                char digits[24];
                int n = 0;
                long long tmp = price <= 0 ? 0 : price;
                if (tmp == 0) digits[n++] = '0';
                while (tmp > 0 && n < 23) {
                    digits[n++] = (char)('0' + (tmp % 10));
                    tmp /= 10;
                }
                // Commas every 3 from the RIGHT — see tooltip formatter.
                int w = 0;
                for (int k = 0; k < n && w < 30; ++k) {
                    priceStr[w++] = digits[n - 1 - k];
                    int left = n - 1 - k;
                    if (left > 0 && left % 3 == 0 && w < 30) priceStr[w++] = ',';
                }
                priceStr[w] = 0;

                char msg[128];
                _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                            "Do you want to buy this damage skin\r\n"
                            "for %s mesos?",
                            priceStr);

                // Hide our tooltip so it doesn't linger behind the modal.
                TooltipHide();
                play_ui_sound(L"BtMouseClick");

                ZXString<char> zmsg(msg);
                int r = 0;
                try { r = CUtilDlg_YesNo(zmsg, nullptr, nullptr, 0, 0); }
                catch (...) { PickerLog("YesNo threw"); r = 0; }

                if (r == kYesNo_Yes) {
                    Send_DamageSkinPurchase(id);
                    PickerLog("Purchase confirmed — skin=%d price=%lld",
                              id, (long long)price);
                } else {
                    PickerLog("Purchase cancelled (r=%d)", r);
                }
            }
            return;
        }
        // Paging — strict page-aligned scroll (no overlap). Old code
        // clamped to `size - kRows` which produced duplicates when the
        // tail page was partial (e.g. 13 items → clamp=4, so page 2 drew
        // indices 4..12 and overlapped page 1's 0..8).
        constexpr int kRows = kGridPerPage;
        if (pressed == 4 && PtInRect(&m_rcMyPrev, pt)) {
            m_nMyScroll = (std::max)(0, m_nMyScroll - kRows);
            InvalidateRect(nullptr); return;
        }
        if (pressed == 5 && PtInRect(&m_rcMyNext, pt)) {
            int next = m_nMyScroll + kRows;
            if (next < (int)m_vMyList.size()) m_nMyScroll = next;
            InvalidateRect(nullptr); return;
        }
        if (pressed == 6 && PtInRect(&m_rcShopPrev, pt)) {
            m_nShopScroll = (std::max)(0, m_nShopScroll - kRows);
            InvalidateRect(nullptr); return;
        }
        if (pressed == 7 && PtInRect(&m_rcShopNext, pt)) {
            int next = m_nShopScroll + kRows;
            if (next < (int)m_vShopList.size()) m_nShopScroll = next;
            InvalidateRect(nullptr); return;
        }
        // Preview-background cycler arrows.
        if (pressed == 10 && PtInRect(&m_rcPrevBgArrow, pt)) {
            m_nPreviewBg = (m_nPreviewBg + 2) % 3;
            InvalidateRect(nullptr); return;
        }
        if (pressed == 11 && PtInRect(&m_rcNextBgArrow, pt)) {
            m_nPreviewBg = (m_nPreviewBg + 1) % 3;
            InvalidateRect(nullptr); return;
        }

        // GO buttons: parse the input field and jump to that page.
        auto jump = [this](std::wstring& buf, int& scroll, int total) {
            if (buf.empty()) return;
            int n = _wtoi(buf.c_str());
            if (n <= 0) { buf.clear(); return; }
            int totalPages = (total + kGridPerPage - 1) / kGridPerPage;
            if (n > totalPages) n = totalPages;
            scroll = (n - 1) * kGridPerPage;
            buf.clear();
        };
        if (pressed == 8 && PtInRect(&m_rcMyGo, pt)) {
            jump(m_sMyInput, m_nMyScroll, (int)m_vMyList.size());
            InvalidateRect(nullptr); return;
        }
        if (pressed == 9 && PtInRect(&m_rcShopGo, pt)) {
            jump(m_sShopInput, m_nShopScroll, (int)m_vShopList.size());
            InvalidateRect(nullptr); return;
        }
    }
}


// ---------------------------------------------------------------------------
// Cash-item hooks — intercept USE request for 5910000, open the picker
// instead of forwarding to server (which would consume the item).
// ---------------------------------------------------------------------------

class CWvsContext;
static auto CWvsContext__SendConsumeCashItemUseRequest =
    reinterpret_cast<void(__thiscall*)(CWvsContext*, int, int, int, ZXString<char>)>(kAddr_SendConsumeCash);

void __fastcall CWvsContext__SendConsumeCashItemUseRequest_hook_picker(
    CWvsContext* pThis, void* /*edx*/, int nPOS, int nItemID, int a4, ZXString<char> a5)
{
    if (nItemID != kCashItem_DamageSkin) {
        CWvsContext__SendConsumeCashItemUseRequest(pThis, nPOS, nItemID, a4, a5);
        return;
    }
    if (CUIDamageSkinPicker::ms_pInstance) {
        CUIDamageSkinPicker::ms_pInstance->Destroy();
        CUIDamageSkinPicker::ms_pInstance = nullptr;
        return;
    }
    PickerLog("SendConsumeCash intercept: opening picker (item=%d pos=%d)",
              nItemID, nPOS);
    new CUIDamageSkinPicker();
    // Intentionally do NOT forward — item is not consumed.
}

static auto get_consume_cash_item_type_picker =
    reinterpret_cast<int32_t(__cdecl*)(int32_t)>(kAddr_get_consume_type);

// ---------------------------------------------------------------------------
// Keyboard routing — intercept CWndMan::ProcessKey so digits, Enter,
// Backspace and Escape can feed the picker's page-input boxes.
// ---------------------------------------------------------------------------

bool CUIDamageSkinPicker::HandleKey(int vk) {
    if (m_nInputFocus == 0) return false;
    std::wstring& buf  = (m_nInputFocus == 1) ? m_sMyInput   : m_sShopInput;
    int&          scr  = (m_nInputFocus == 1) ? m_nMyScroll  : m_nShopScroll;
    int           sz   = (m_nInputFocus == 1) ? (int)m_vMyList.size()
                                              : (int)m_vShopList.size();

    auto appendDigit = [&](wchar_t c) {
        if (buf.length() < 2) buf += c;   // max 2 digits (up to page 99)
    };

    if (vk >= '0' && vk <= '9') {
        appendDigit((wchar_t)vk);
        InvalidateRect(nullptr); return true;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        appendDigit((wchar_t)('0' + vk - VK_NUMPAD0));
        InvalidateRect(nullptr); return true;
    }
    if (vk == VK_BACK) {
        if (!buf.empty()) buf.pop_back();
        InvalidateRect(nullptr); return true;
    }
    if (vk == VK_RETURN) {
        if (!buf.empty()) {
            int n = _wtoi(buf.c_str());
            int totalPages = (sz + kGridPerPage - 1) / kGridPerPage;
            if (n > totalPages) n = totalPages;
            if (n > 0) scr = (n - 1) * kGridPerPage;
            buf.clear();
        }
        InvalidateRect(nullptr); return true;
    }
    if (vk == VK_ESCAPE) {
        m_nInputFocus = 0;
        InvalidateRect(nullptr); return true;
    }
    return false;
}

// CWndMan::ProcessKey(msg, vk, lParam) — v95 sym, v83 VA 0x009E40F1 (ret 0Ch). Return value ignored by callers — we
// control forwarding via whether we call the original.
static auto CWndMan__ProcessKey =
    reinterpret_cast<int(__thiscall*)(void*, unsigned, unsigned, int)>(0x009E40F1);

int __fastcall CWndMan__ProcessKey_hook(
    void* pThis, void* /*edx*/, unsigned a2, unsigned a3, int a4)
{
    // Keydown = transition state bit (0x80000000) CLEAR. v83's a4 layout
    // mirrors Win32 KEYDOWN lParam: bits 16-23 scan code, bit 30 = "was
    // previously down" (repeat), bit 31 = "transition state" (1 = release).
    bool keydown = (a4 & 0x80000000u) == 0;

    static int s_nLogs = 0;
    if (keydown && CUIDamageSkinPicker::ms_pInstance &&
        CUIDamageSkinPicker::ms_pInstance->InputFocus() != 0 &&
        s_nLogs < 30)
    {
        ++s_nLogs;
        PickerLog("ProcessKey: a3=0x%X a4=0x%X focus=%d",
                  a3, a4, CUIDamageSkinPicker::ms_pInstance->InputFocus());
    }

    if (keydown && CUIDamageSkinPicker::ms_pInstance) {
        if (CUIDamageSkinPicker::ms_pInstance->HandleKey((int)a3)) {
            return 0;
        }
    }
    return CWndMan__ProcessKey(pThis, a2, a3, a4);
}

int32_t __cdecl get_consume_cash_item_type_hook_picker(int32_t nItemID) {
    if (nItemID == kCashItem_DamageSkin) return 1;
    return get_consume_cash_item_type_picker(nItemID);
}


// ---------------------------------------------------------------------------
// Attach
// ---------------------------------------------------------------------------

void AttachDamageSkinPickerMod() {
    PickerLog("AttachDamageSkinPickerMod: begin");
    ATTACH_HOOK(CWvsContext__SendConsumeCashItemUseRequest,
                CWvsContext__SendConsumeCashItemUseRequest_hook_picker);
    ATTACH_HOOK(get_consume_cash_item_type_picker,
                get_consume_cash_item_type_hook_picker);
    ATTACH_HOOK(CWndMan__ProcessKey, CWndMan__ProcessKey_hook);
    PickerLog("AttachDamageSkinPickerMod: hooks attached for item %d",
              kCashItem_DamageSkin);
}
