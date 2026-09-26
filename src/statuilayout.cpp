#include "pch.h"
#include "hook.h"

#include <cstring>

// ============================================================
// Stat window geometry for the Data\UI\UIWindow.img Stat skin.
//
//   Stat/backgrnd   279 x 365  (stock 176 x 348). Adds a REBIRTH row at y=186, so every
//                   row from ABILITY POINT down sits 18px (one row) lower than stock.
//   Stat/backgrnd2  260 x 149  (stock 177 x 203). Two columns; statdetaillayout.cpp
//                   moves the values into it.
//
// Rows NAME..FAME are unchanged (values at y 33..169 on plates 32..168). Right-edge
// controls keep their stock inset from the right border: +103 on a 279-wide skin.
//
// Every site is a 4-byte immediate checked against its stock value before writing, so
// a mismatched exe leaves that one coordinate stock instead of writing into code.
//
// Deliberately NOT moved: BtAuto/BtAuto1/BtAuto2 X (`push 61h`) and BtDetail X
// (`push 7Ch`) are imm8 pushes, so they cannot exceed 127 in place. They stay at 97 /
// 124, beside the AP box and in the footer. The HP/MP AP-up Y (`push 75h` / `push 87h`)
// are above the REBIRTH row and already line up.
// ============================================================

namespace {

constexpr int kRowShift   = 18;          // one 18px row: REBIRTH
constexpr int kMainWidth  = 279;         // Stat/backgrnd
constexpr int kMainHeight = 365;
constexpr int kRightShift = 103;         // kMainWidth - stock 176

// CUIStatDetail geometry — must match Stat/backgrnd2.
constexpr int kDetailWidth  = 260;
constexpr int kDetailHeight = 149;
// Panel corner relative to CUIStat: X = mainWidth - 5 merges the two frames exactly as
// stock 170 does on a 175-wide skin; Y = mainHeight - detailHeight bottom-aligns them.
constexpr int kDetailX = kMainWidth - 5;               // 274
constexpr int kDetailY = kMainHeight - kDetailHeight;  // 216

struct Imm32Site {
    uintptr_t   uAddress;    // address of the imm32 itself
    int         nStock;
    int         nValue;
    const char* sWhat;
};

const Imm32Site kSites[] = {
    // --- CUIStat::CUIStat (0x008C4842) ---
    { 0x008C4AAE + 1, 0x15C, kMainHeight,               "CUIStat::CUIStat — window height" },
    { 0x008C4AB3 + 1, 0x0B0, kMainWidth,                "CUIStat::CUIStat — window width" },
    { 0x008C485A + 1, 0x09C, 0x09C + kRightShift,       "CUIStat::CUIStat — close button X" },

    // --- CUIStat::OnCreate (0x008C4C7F) ---
    { 0x008C4E1B + 1, 0x144, 0x144 + kRowShift,         "CUIStat::OnCreate — BtDetail Y" },

    // --- CUIStat::Draw (0x008C59FF) ---
    { 0x008C6564 + 1, 0x0F4, 0x0F4 + kRowShift,         "CUIStat::Draw — STR value Y" },
    { 0x008C66FD + 1, 0x106, 0x106 + kRowShift,         "CUIStat::Draw — DEX value Y" },
    { 0x008C6896 + 1, 0x118, 0x118 + kRowShift,         "CUIStat::Draw — INT value Y" },
    { 0x008C6A2A + 1, 0x12A, 0x12A + kRowShift,         "CUIStat::Draw — LUK value Y" },
    { 0x008C6B65 + 1, 0x0D7, 0x0D7 + kRowShift,         "CUIStat::Draw — AP value Y (right-aligned at x=85)" },
    { 0x008C63F6 + 3, 0x0F4, 0x0F4 + kRowShift,         "CUIStat::Draw — Disabled/<stat> label Y base (lea eax,[eax+eax+0F4h])" },
    { 0x008C62AF + 1, 0x0C3, 0x0C3 + kRowShift,         "CUIStat::Draw — beginner Lv<=10 canvas Y" },

    // --- CUIStat::RestoreButtons (0x008C79F5) ---
    { 0x008C7AD9 + 1, 0x099, 0x099 + kRightShift,       "CUIStat::RestoreButtons — BtApUp X (all six)" },
    { 0x008C7BCC + 1, 0x0F7, 0x0F7 + kRowShift,         "CUIStat::RestoreButtons — BtApUp STR Y" },
    { 0x008C7C24 + 1, 0x109, 0x109 + kRowShift,         "CUIStat::RestoreButtons — BtApUp DEX Y" },
    { 0x008C7C7C + 1, 0x11B, 0x11B + kRowShift,         "CUIStat::RestoreButtons — BtApUp INT Y" },
    { 0x008C7CD4 + 1, 0x12D, 0x12D + kRowShift,         "CUIStat::RestoreButtons — BtApUp LUK Y" },
    { 0x008C7CEB + 1, 0x0C5, 0x0C5 + kRowShift,         "CUIStat::RestoreButtons — BtAuto / BtAuto1 Y" },
    { 0x008C7ED8 + 1, 0x0D9, 0x0D9 + kRowShift,         "CUIStat::RestoreButtons — BtAuto2 Y" },

    // --- CUIStat::OnMouseMove (0x008C52D7): EXP hover right edge, to the wider field ---
    { 0x008C5339 + 3, 0x0AC, 0x0AC + kRightShift,       "CUIStat::OnMouseMove — EXP hover right edge" },

    // --- sub_8C6C84 (called only from CUIStat::OnCreate): AP tutorial tips, (x, y) ---
    // The one at x=0xAA points at AUTO-ASSIGN, which does not move sideways; the rest
    // point at the BtApUp column (x 0x99 / 0xA4 = its left / right edge) and follow it.
    { 0x008C6D3A + 1, 0x0C5, 0x0C5 + kRowShift,         "AP tip 1 Y (AUTO-ASSIGN)" },
    { 0x008C6E9C + 1, 0x0ED, 0x0ED + kRowShift,         "AP tip 2 Y" },
    { 0x008C6EA1 + 1, 0x099, 0x099 + kRightShift,       "AP tip 2 X" },
    { 0x008C6F77 + 1, 0x0FF, 0x0FF + kRowShift,         "AP tip 3 Y" },
    { 0x008C6F7C + 1, 0x0A4, 0x0A4 + kRightShift,       "AP tip 3 X" },
    { 0x008C7090 + 1, 0x123, 0x123 + kRowShift,         "AP tip 4 Y" },
    { 0x008C7095 + 1, 0x0A4, 0x0A4 + kRightShift,       "AP tip 4 X" },
    { 0x008C71AF + 1, 0x0FF, 0x0FF + kRowShift,         "AP tip 5 Y" },
    { 0x008C71B4 + 1, 0x0A4, 0x0A4 + kRightShift,       "AP tip 5 X" },
    { 0x008C72CE + 1, 0x111, 0x111 + kRowShift,         "AP tip 6 Y" },
    { 0x008C72D3 + 1, 0x0A4, 0x0A4 + kRightShift,       "AP tip 6 X" },
    { 0x008C73ED + 1, 0x0F8, 0x0F8 + kRowShift,         "AP tip 7 Y" },
    { 0x008C73F2 + 1, 0x0A4, 0x0A4 + kRightShift,       "AP tip 7 X" },
    { 0x008C7506 + 1, 0x0F8, 0x0F8 + kRowShift,         "AP tip 8 Y" },
    { 0x008C750B + 1, 0x0A4, 0x0A4 + kRightShift,       "AP tip 8 X" },
    { 0x008C763F + 1, 0x0F8, 0x0F8 + kRowShift,         "AP tip 9 Y" },
    { 0x008C7644 + 1, 0x0A4, 0x0A4 + kRightShift,       "AP tip 9 X" },
    { 0x008C7752 + 1, 0x123, 0x123 + kRowShift,         "AP tip 10 Y" },
    { 0x008C7757 + 1, 0x0A4, 0x0A4 + kRightShift,       "AP tip 10 X" },
    { 0x008C7871 + 1, 0x0FF, 0x0FF + kRowShift,         "AP tip 11 Y" },
    { 0x008C7876 + 1, 0x0A4, 0x0A4 + kRightShift,       "AP tip 11 X" },
    { 0x008C799C + 1, 0x111, 0x111 + kRowShift,         "AP tip 12 Y" },
    { 0x008C79A1 + 1, 0x0A4, 0x0A4 + kRightShift,       "AP tip 12 X" },

    // --- CUIStatDetail::CUIStatDetail (0x008C50B4): CreateWnd(x, y, w, h, ...) ---
    { 0x008C5105 + 1, 0x0CB, kDetailHeight,             "CUIStatDetail ctor — height" },
    { 0x008C510A + 1, 0x0B1, kDetailWidth,              "CUIStatDetail ctor — width" },

    // --- Detail panel corner = CUIStat corner + (X, Y). All seven sites must agree or
    // the panel jumps between positions when opened / dragged. ---
    { 0x008C4EA2 + 1, 0x0AA, kDetailX,                  "CUIStat::OnCreate — detail X" },
    { 0x008C4E91 + 1, 0x090, kDetailY,                  "CUIStat::OnCreate — detail Y" },
    { 0x008C54A9 + 1, 0x090, kDetailY,                  "CUIStat::ToggleDetail — detail Y" },
    { 0x008C5760 + 1, 0x0AA, kDetailX,                  "sub_8C5531 — detail X (drag anchor)" },
    // Pairs with the add above: `neg; sbb eax,eax; and eax,-170; add eax,170` yields 170
    // or exactly 0, so the mask must move with the add or the "0" arm becomes nonzero.
    { 0x008C575B + 1, -0x0AA, -kDetailX,                "sub_8C5531 — detail X mask" },
    { 0x008C57BF + 1, 0x090, kDetailY,                  "sub_8C5531 — detail Y (drag anchor)" },
    { 0x008C6C72 + 1, 0x0AA, kDetailX,                  "sub_8C6C3F — detail X (drag follow)" },
    { 0x008C6C65 + 1, 0x090, kDetailY,                  "sub_8C6C3F — detail Y (drag follow)" },
};

} // namespace

void AttachStatUiLayoutMod() {
    // All or nothing: a half-moved window (values on one grid, buttons on another) is
    // harder to diagnose than a stock one.
    for (const Imm32Site& site : kSites) {
        if (memcmp(reinterpret_cast<void*>(site.uAddress), &site.nStock, sizeof(int)) != 0) {
            ErrorMessage("Stat UI layout: %s at 0x%08X is not the stock %d - skipping the whole "
                         "stat window relayout.", site.sWhat, site.uAddress, site.nStock);
            return;
        }
    }
    for (const Imm32Site& site : kSites) {
        Patch4(site.uAddress, static_cast<unsigned int>(site.nValue));
    }
}
