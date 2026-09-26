#include "pch.h"
#include "hook.h"


// Key Config quickslot dialog (CQuickslotKeyModifyDlg): 8 -> 26 slots, to match longkeyboard.cpp.
// Needs the 581x238 UI/UIWindow.img/KeyConfig/quickslotConfig/backgrnd (13 x 2 slot well).
//
// Stock layout (0xFC bytes): CDialog | int m_aKey[8] @+0x7C | focus region @+0x9C (-1 none, 0 slots,
// 1 OK/Cancel) | cur slot @+0xA0 | cur button @+0xA4 | ZRef<CCtrlButton> m_aSlot[8] @+0xA8 |
// ZRef OK @+0xE8 | ZRef Cancel @+0xF0 | canvas @+0xF8.
// The keys keep +0x7C (all its references are disp8) and grow into the space behind it; everything
// after them moves to the end of a larger allocation. Every reference to those members is disp32,
// so moving them is a table of verified displacement rewrites.

constexpr int kSlots = 26;
constexpr int kColumns = 13;
constexpr int kDlgSize = 0x1D4;           // 0xFC + 18 more keys (0x48) + 18 more slot ZRefs (0x90)
constexpr int kWiden = (kColumns - 4) * 35; // backgrnd is 315 px wider than stock

struct MemberDisp {
    uintptr_t uAddress; // address of the disp32 inside the instruction
    unsigned int uOld;
    unsigned int uNew;
};

// Generated from the disassembly of 0x0072CA6B-0x0072DFF3 (all functions of the dialog): every memory
// displacement AND every immediate in the member range (Draw reaches +0xF8 twice through add eax,0F8h).
// Displacements are relative to each function's base register: this, or this+4 (OnKey, OnMouseButton,
// reached through the IUIMsgHandler vtable 0xAFDA2C) or this+8 (dtor body). Remap: +0x9C..+0xE7 -> +0x48,
// +0xE8..+0xFB -> +0xD8. Excluded: Draw's push [eax+0ACh] at 0x0072D867 (SKILLENTRY, not a member).
static const MemberDisp g_aMemberDisp[] = {
    {0x0072CA85, 0x09C, 0x0E4}, // ctor                   or dword ptr [esi + 0x9c], 0xffffffff
    {0x0072CA9C, 0x0A8, 0x0F0}, // ctor                   lea eax, [esi + 0xa8]
    {0x0072CAA6, 0x0A0, 0x0E8}, // ctor                   mov dword ptr [esi + 0xa0], edi
    {0x0072CAAC, 0x0A4, 0x0EC}, // ctor                   mov dword ptr [esi + 0xa4], edi
    {0x0072CAB7, 0x0EC, 0x1C4}, // ctor                   mov dword ptr [esi + 0xec], edi
    {0x0072CABD, 0x0F4, 0x1CC}, // ctor                   mov dword ptr [esi + 0xf4], edi
    {0x0072CAC3, 0x0F8, 0x1D0}, // ctor                   mov dword ptr [esi + 0xf8], edi
    {0x0072CB98, 0x0F0, 0x1C8}, // dtor body (this+8)     lea edi, [esi + 0xf0]
    {0x0072CBB7, 0x0E8, 0x1C0}, // dtor body (this+8)     lea ecx, [esi + 0xe8]
    {0x0072CBC6, 0x0E0, 0x1B8}, // dtor body (this+8)     lea ecx, [esi + 0xe0]
    {0x0072CBE2, 0x0A0, 0x0E8}, // dtor body (this+8)     lea eax, [esi + 0xa0]
    {0x0072CC4D, 0x0AC, 0x0F4}, // OnCreate               lea eax, [esi + 0xac]
    {0x0072CD7F, 0x0E8, 0x1C0}, // OnCreate               lea ecx, [esi + 0xe8]
    {0x0072CD8A, 0x0EC, 0x1C4}, // OnCreate               mov ecx, dword ptr [esi + 0xec]
    {0x0072CE10, 0x0F0, 0x1C8}, // OnCreate               lea ecx, [esi + 0xf0]
    {0x0072CE1B, 0x0F4, 0x1CC}, // OnCreate               mov ecx, dword ptr [esi + 0xf4]
    {0x0072CED9, 0x098, 0x0E0}, // OnKey (this+4)         cmp dword ptr [esi + 0x98], 0
    {0x0072CEFB, 0x098, 0x0E0}, // OnKey (this+4)         cmp dword ptr [esi + 0x98], 0
    {0x0072CF08, 0x09C, 0x0E4}, // OnKey (this+4)         mov eax, dword ptr [esi + 0x9c]
    {0x0072CF25, 0x098, 0x0E0}, // OnKey (this+4)         mov eax, dword ptr [esi + 0x98]
    {0x0072CF38, 0x09C, 0x0E4}, // OnKey (this+4)         mov eax, dword ptr [esi + 0x9c]
    {0x0072CF5D, 0x098, 0x0E0}, // OnKey (this+4)         cmp dword ptr [esi + 0x98], 0
    {0x0072CF6A, 0x09C, 0x0E4}, // OnKey (this+4)         mov eax, dword ptr [esi + 0x9c]
    {0x0072CF86, 0x098, 0x0E0}, // OnKey (this+4)         mov eax, dword ptr [esi + 0x98]
    {0x0072CF99, 0x09C, 0x0E4}, // OnKey (this+4)         mov eax, dword ptr [esi + 0x9c]
    {0x0072CFD2, 0x098, 0x0E0}, // OnKey (this+4)         or dword ptr [esi + 0x98], 0xffffffff
    {0x0072CFDB, 0x098, 0x0E0}, // OnKey (this+4)         cmp dword ptr [esi + 0x98], -1
    {0x0072CFE2, 0x098, 0x0E0}, // OnKey (this+4)         lea edi, [esi + 0x98]
    {0x0072CFF5, 0x0A0, 0x0E8}, // OnKey (this+4)         and dword ptr [esi + 0xa0], 0
    {0x0072CFFC, 0x09C, 0x0E4}, // OnKey (this+4)         and dword ptr [esi + 0x9c], 0
    {0x0072D070, 0x09C, 0x0E4}, // OnButtonClicked        and dword ptr [esi + 0x9c], 0
    {0x0072D07D, 0x0A0, 0x0E8}, // OnButtonClicked        lea edi, [esi + 0xa0]
    {0x0072D08D, 0x0AC, 0x0F4}, // OnButtonClicked        mov ecx, dword ptr [esi + eax*8 + 0xac]
    {0x0072D14E, 0x098, 0x0E0}, // OnMouseButton (this+4) or dword ptr [esi + 0x98], 0xffffffff
    {0x0072D186, 0x0F8, 0x1D0}, // Draw                   lea ecx, [esi + 0xf8]
    {0x0072D237, 0x0F8, 0x1D0}, // Draw                   lea edi, [esi + 0xf8]
    {0x0072D2F8, 0x0F8, 0x1D0}, // Draw                   cmp dword ptr [esi + 0xf8], ebx
    {0x0072D2AA, 0x0F8, 0x1D0}, // Draw                   add eax, 0xf8 (imm32; eax = this from [ebp-1Ch])
    {0x0072D2FE, 0x0F8, 0x1D0}, // Draw                   lea eax, [esi + 0xf8]
    {0x0072D323, 0x0F8, 0x1D0}, // Draw                   add eax, 0xf8 (imm32; eax = this from [ebp-1Ch])
    {0x0072D366, 0x0F8, 0x1D0}, // Draw                   cmp dword ptr [eax + 0xf8], ebx
    {0x0072D36C, 0x0F8, 0x1D0}, // Draw                   lea esi, [eax + 0xf8]
    {0x0072DDBE, 0x09C, 0x0E4}, // focus: Tab             mov eax, dword ptr [esi + 0x9c]
    {0x0072DDD4, 0x0EC, 0x1C4}, // focus: Tab             mov ecx, dword ptr [esi + 0xec]
    {0x0072DDE0, 0x0A4, 0x0EC}, // focus: Tab             and dword ptr [esi + 0xa4], 0
    {0x0072DDE7, 0x09C, 0x0E4}, // focus: Tab             mov dword ptr [esi + 0x9c], edi
    {0x0072DDEF, 0x09C, 0x0E4}, // focus: Tab             or dword ptr [esi + 0x9c], 0xffffffff
    {0x0072DDF8, 0x0AC, 0x0F4}, // focus: Tab             mov ecx, dword ptr [esi + 0xac]
    {0x0072DE04, 0x09C, 0x0E4}, // focus: Tab             and dword ptr [esi + 0x9c], 0
    {0x0072DE0B, 0x0A0, 0x0E8}, // focus: Tab             and dword ptr [esi + 0xa0], 0
    {0x0072DE1E, 0x0A0, 0x0E8}, // focus: MoveSlot        mov eax, dword ptr [esi + 0xa0]
    {0x0072DE2D, 0x0A0, 0x0E8}, // focus: MoveSlot        and dword ptr [esi + 0xa0], 0
    {0x0072DE34, 0x09C, 0x0E4}, // focus: MoveSlot        cmp dword ptr [esi + 0x9c], 0
    {0x0072DE55, 0x0A0, 0x0E8}, // focus: MoveSlot        add eax, dword ptr [esi + 0xa0]
    {0x0072DE65, 0x0AC, 0x0F4}, // focus: MoveSlot        mov ecx, dword ptr [esi + edi*8 + 0xac]
    {0x0072DE72, 0x0A0, 0x0E8}, // focus: MoveSlot        mov dword ptr [esi + 0xa0], edi
    {0x0072DE86, 0x0A4, 0x0EC}, // focus: OK/Cancel       mov eax, dword ptr [esi + 0xa4]
    {0x0072DE97, 0x0A4, 0x0EC}, // focus: OK/Cancel       and dword ptr [esi + 0xa4], 0
    {0x0072DE9E, 0x09C, 0x0E4}, // focus: OK/Cancel       cmp dword ptr [esi + 0x9c], edi
    {0x0072DEAD, 0x0F4, 0x1CC}, // focus: OK/Cancel       mov ecx, dword ptr [esi + 0xf4]
    {0x0072DEB9, 0x0A4, 0x0EC}, // focus: OK/Cancel       mov dword ptr [esi + 0xa4], edi
    {0x0072DEC8, 0x0EC, 0x1C4}, // focus: OK/Cancel       mov ecx, dword ptr [esi + 0xec]
    {0x0072DED4, 0x0A4, 0x0EC}, // focus: OK/Cancel       and dword ptr [esi + 0xa4], 0
    {0x0072DEE7, 0x0AC, 0x0F4}, // ClearAllFocus          lea esi, [edi + 0xac]
    {0x0072DEFD, 0x0EC, 0x1C4}, // ClearAllFocus          mov ecx, dword ptr [edi + 0xec]
    {0x0072DF0A, 0x0F4, 0x1CC}, // ClearAllFocus          mov ecx, dword ptr [edi + 0xf4]
    {0x0072DFDC, 0x0A0, 0x0E8}, // ModifyQuickslotKeyMap  mov eax, dword ptr [edi + 0xa0]
};

// New member offsets (this-relative), for the C++ hooks below
constexpr int kFocusRegion = 0xE4; // -1 none, 0 slots, 1 OK/Cancel
constexpr int kCurSlot = 0xE8;

static int& DlgInt(void* pDlg, int nOffset) {
    return *reinterpret_cast<int*>(static_cast<unsigned char*>(pDlg) + nOffset);
}

// focus helpers (thiscall on the dialog, ret 4)
static auto CQuickslotKeyModifyDlg__MoveSlotFocus = reinterpret_cast<void(__thiscall*)(void*, int)>(0x0072DE13);   // delta -1/+1/-13/+13
static auto CQuickslotKeyModifyDlg__MoveButtonFocus = reinterpret_cast<void(__thiscall*)(void*, int)>(0x0072DE7B); // VK_LEFT / VK_RIGHT

// CQuickslotKeyModifyDlg::GetSlotPos — v95 sym, v83 VA 0x0072DF19 (ret 0Ch). Stock: 4 columns at x 0x41 + 35c,
// rows at y 0x71 / 0x93. Replaced: 13 columns.
static auto CQuickslotKeyModifyDlg__GetSlotPos = reinterpret_cast<void(__thiscall*)(void*, int, int*, int*)>(0x0072DF19);

void __fastcall CQuickslotKeyModifyDlg__GetSlotPos_hook(void* pThis, void* _EDX, int nIdx, int* pnX, int* pnY) {
    *pnX = 0x41 + 35 * (nIdx % kColumns);
    *pnY = nIdx < kColumns ? 0x71 : 0x93;
}

// CQuickslotKeyModifyDlg::OnKey — v95 sym, v83 VA 0x0072CE91, IUIMsgHandler thunk (ecx = this+4, ret 8).
// The stock arrow-key navigation keeps the column count (4) in ecx together with the Enter decode, so the
// arrows are handled here for a 13 x 2 grid; every other key goes to the stock code.
static auto CQuickslotKeyModifyDlg__OnKey = reinterpret_cast<void(__thiscall*)(void*, unsigned int, unsigned int)>(0x0072CE91);

void __fastcall CQuickslotKeyModifyDlg__OnKey_hook(void* pThis4, void* _EDX, unsigned int nKey, unsigned int nFlags) {
    if ((nFlags & 0x80000000) || nKey < VK_LEFT || nKey > VK_DOWN) {
        CQuickslotKeyModifyDlg__OnKey(pThis4, nKey, nFlags);
        return;
    }
    void* pDlg = static_cast<unsigned char*>(pThis4) - 4;
    int& nRegion = DlgInt(pDlg, kFocusRegion);
    int nCur = DlgInt(pDlg, kCurSlot);
    switch (nKey) {
    case VK_LEFT:
    case VK_RIGHT:
        if (nRegion == -1) {
            return;
        }
        if (nRegion == 0) {
            bool bEdge = nKey == VK_LEFT ? nCur % kColumns == 0 : nCur % kColumns == kColumns - 1;
            if (!bEdge) {
                CQuickslotKeyModifyDlg__MoveSlotFocus(pDlg, nKey == VK_LEFT ? -1 : 1);
            }
        } else if (nRegion == 1) {
            CQuickslotKeyModifyDlg__MoveButtonFocus(pDlg, nKey);
        } else {
            nRegion = -1;
        }
        return;
    case VK_UP:
        if (nRegion == 0 && nCur >= kColumns && nCur < kSlots) {
            CQuickslotKeyModifyDlg__MoveSlotFocus(pDlg, -kColumns);
        }
        return;
    case VK_DOWN:
        if (nRegion == 0 && nCur >= 0 && nCur < kColumns) {
            CQuickslotKeyModifyDlg__MoveSlotFocus(pDlg, kColumns);
        }
        return;
    }
}


void AttachQuickslotDlgMod() {
    for (const MemberDisp& d : g_aMemberDisp) {
        if (*reinterpret_cast<unsigned int*>(d.uAddress) != d.uOld) {
            LogMessage("quickslotdlg: unexpected disp 0x%X at 0x%08X, mod not applied", *reinterpret_cast<unsigned int*>(d.uAddress), d.uAddress);
            ErrorMessage("CQuickslotKeyModifyDlg: unexpected disp 0x%X at 0x%08X, mod not applied", *reinterpret_cast<unsigned int*>(d.uAddress), d.uAddress);
            return; // all or nothing: a partial relocation would corrupt the dialog
        }
    }
    for (const MemberDisp& d : g_aMemberDisp) {
        Patch4(d.uAddress, d.uNew);
    }

    Patch4(0x00836CA8 + 1, kDlgSize);          // sub_836C9B (Key Config -> Quick Slot): alloc 0FCh -> 1D4h
    Patch1(0x00836D13 + 1, kSlots * 4);        // sub_836C9B: copy dlg keys (+7Ch) back to CQuickslotKeyMappedMan, 20h -> 68h
    Patch1(0x0072CA94 + 1, kSlots);            // CQuickslotKeyModifyDlg ctor: construct 8 -> 26 slot ZRefs
    Patch1(0x0072CBDC + 1, kSlots);            // dtor body: destruct 8 -> 26 slot ZRefs
    Patch1(0x0072CC2D + 2, 4 + kSlots * 4);    // OnCreate: copy keys from CQuickslotKeyMappedMan, cmp edx,24h -> 6Ch
    Patch1(0x0072CD0B + 3, kSlots);            // OnCreate: create 8 -> 26 slot buttons (ids 3E8h+i)
    Patch4(0x0072CD9B + 1, 0x9D + kWiden);     // OnCreate: OK button x (right-aligned like stock)
    Patch4(0x0072CE2C + 1, 0xD2 + kWiden);     // OnCreate: Cancel button x
    Patch4(0x0072D067 + 1, 0x3E8 + kSlots - 1); // OnButtonClicked: slot ids 3E8h..3EFh -> 3E8h..401h
    Patch4(0x0072D11B + 3, 0x41 + 35 * kColumns - 3); // OnMouseButton: slot area right edge 0CAh -> 205h
    Patch1(0x0072DD60 + 3, kSlots);            // Draw: key-label loop 8 -> 26
    Patch1(0x0072DE26 + 2, kSlots - 1);        // MoveSlotFocus: clamp cur slot 7 -> 25
    Patch1(0x0072DE49 + 2, static_cast<unsigned char>(-kColumns)); // MoveSlotFocus: accepted delta -4 -> -13
    Patch1(0x0072DE4E + 2, kColumns);          // MoveSlotFocus: accepted delta 4 -> 13
    Patch1(0x0072DE5D + 2, kSlots - 1);        // MoveSlotFocus: clamp new slot 7 -> 25
    Patch1(0x0072DEE3 + 1, kSlots);            // ClearAllFocus: 8 -> 26 slot buttons
    Patch1(0x0072E1F4 + 2, kSlots);            // IsUsedKey: 8 -> 26 keys

    ATTACH_HOOK(CQuickslotKeyModifyDlg__GetSlotPos, CQuickslotKeyModifyDlg__GetSlotPos_hook);
    ATTACH_HOOK(CQuickslotKeyModifyDlg__OnKey, CQuickslotKeyModifyDlg__OnKey_hook);
    LogMessage("quickslotdlg: applied (%u member offsets, size 0x%X)", static_cast<unsigned int>(sizeof(g_aMemberDisp) / sizeof(g_aMemberDisp[0])), kDlgSize);
}
