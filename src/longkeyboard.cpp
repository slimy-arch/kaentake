#include "pch.h"
#include "hook.h"


// Long Keyboard: status-bar quickslot 8 (4x2) -> 26 (13x2) keys.
// Port of Andre-3001's "Long Keyboard v83" (gist d7b0d5c17cf36856f820c6fae8cf1022), re-verified against
// this exe. Server contract: CP 0xB7 / LP 0x9F carry 26 ints (Cosmic QuickslotBinding.QUICKSLOT_SIZE).
// The Key Config quickslot dialog (CQuickslotKeyModifyDlg) still edits keys 1-8 only.

constexpr int kSlots = 26;
constexpr int kBarWidth = 7 + 12 * 35 + 32 + 7; // 466: 13 columns 35px apart, 32px icons, 7px margins
// Keep the stock left edge (647) and grow rightwards: anything left of it sits under the status-bar
// layers. The bar needs 647 + 466 = 1113 px of the 800x600 frame (+152 at widths > 800, see
// CWndMan::ResetOrgWindow), so all 13 columns only show at wide resolutions.
constexpr int kBarX = 0x287;

// CQuickslotKeyMappedMan grows from { vtbl, int[8] @+4, int[8] @+0x24 } (0x44) to
// { vtbl, int[26] @+4, int[26] @+0x6C } (0xD4).
constexpr int kKeysOffset = 4;
constexpr int kBackupOffset = kKeysOffset + kSlots * 4; // 0x6C

// DIK scan codes. Row 1: Shift Ins Home PgUp 1 2 3 4 5 A S D F / Row 2: Ctrl Del End PgDn Q W E R T Z X C V.
// Must match Cosmic's QuickslotBinding.DEFAULT_QUICKSLOTS byte for byte.
static const int g_aDefaultQKM[kSlots] = {
    42, 82, 71, 73, 2, 3, 4, 5, 6, 30, 31, 32, 33,
    29, 83, 79, 81, 16, 17, 18, 19, 20, 44, 45, 46, 47,
};

struct ShortKeyPos {
    int x;
    int y;
};
// s_ptShortKeyPos replacement: slot i at column i % 13, row i / 13.
static ShortKeyPos g_aShortKeyPos[kSlots + 1]; // +1: GetShortCutIndexByPos compares against &[kSlots].y

// CUIStatusBar per-slot state, relocated out of the object (CUIStatusBar+0xD20 is 8 x 12 bytes,
// +0xDC4 is int[8]; neither can grow in place). CUIStatusBar is a singleton (0x00BEC208).
static unsigned char g_abQsCache[kSlots * 12]; // cached FUNCKEY_MAPPED per slot (type byte, id at +1)
static int g_anQsCooltime[kSlots];             // last drawn cooldown index per slot


// CQuickslotKeyMappedMan::CQuickslotKeyMappedMan — v95 sym, v83 VA 0x0072B7BC (stock copies 8 defaults twice)
static auto CQuickslotKeyMappedMan__ctor = reinterpret_cast<void*(__thiscall*)(void*)>(0x0072B7BC);

void* __fastcall CQuickslotKeyMappedMan__ctor_hook(void* pThis, void* _EDX) {
    CQuickslotKeyMappedMan__ctor(pThis);
    auto p = static_cast<unsigned char*>(pThis);
    memcpy(p + kKeysOffset, g_aDefaultQKM, sizeof(g_aDefaultQKM));
    memcpy(p + kBackupOffset, g_aDefaultQKM, sizeof(g_aDefaultQKM));
    return pThis;
}

// CQuickslotKeyMappedMan::DefaultQuickslotKeyMap — v95 sym, v83 VA 0x0072B8E6 (replaced, not called)
static auto CQuickslotKeyMappedMan__DefaultQuickslotKeyMap = reinterpret_cast<void(__thiscall*)(void*)>(0x0072B8E6);

void __fastcall CQuickslotKeyMappedMan__DefaultQuickslotKeyMap_hook(void* pThis, void* _EDX) {
    memcpy(static_cast<unsigned char*>(pThis) + kKeysOffset, g_aDefaultQKM, sizeof(g_aDefaultQKM));
}

// CUIStatusBar::CUIStatusBar — v95 sym, v83 VA 0x008CFAD8. The stock ctor zeroes +0xD20 and fills
// +0xDC4 with -1; do the same for the relocated arrays so nothing survives a channel change.
static auto CUIStatusBar__ctor = reinterpret_cast<void*(__thiscall*)(void*)>(0x008CFAD8);

void* __fastcall CUIStatusBar__ctor_hook(void* pThis, void* _EDX) {
    CUIStatusBar__ctor(pThis);
    memset(g_abQsCache, 0, sizeof(g_abQsCache));
    memset(g_anQsCooltime, 0xFF, sizeof(g_anQsCooltime));
    return pThis;
}

// sub_9FA0CB (creates the CQuickslotKeyMappedMan singleton): push 44h; mov ecx,0BF0B00h -> alloc 0xD4
static auto QkmAlloc_ret = 0x009FA0E6;
void __declspec(naked) QkmAlloc_hook() {
    __asm {
        push    0xD4
        mov     ecx, 0x00BF0B00                         ; ZAllocEx<ZAllocAnonSelector>::_s_alloc
        jmp     [ QkmAlloc_ret ]
    }
}

// CQuickSlot::CompareValidateFuncKeyMappedInfo (0x008DD860): memset(cache, 0, 60h) -> 138h
static auto CacheMemset_ret = 0x008DD8BD;
void __declspec(naked) CacheMemset_hook() {
    __asm {
        push    0x138                                   ; kSlots * 12
        push    0
        push    eax
        jmp     [ CacheMemset_ret ]
    }
}

// Replace an lea/add with "mov reg, imm32" (5 bytes) and nop the rest of the original instruction(s).
static void PatchMovImm(uintptr_t uAddress, unsigned char uOpcode, uintptr_t uValue, size_t uSize) {
    Patch1(uAddress, uOpcode);
    Patch4(uAddress + 1, static_cast<unsigned int>(uValue));
    PatchNop(uAddress + 5, uAddress + uSize);
}


void AttachLongKeyboardMod() {
    for (int i = 0; i < kSlots; ++i) {
        g_aShortKeyPos[i] = {7 + 35 * (i % 13), 8 + 33 * (i / 13)};
    }
    const auto uCache = reinterpret_cast<uintptr_t>(g_abQsCache);
    const auto uPosX = reinterpret_cast<uintptr_t>(&g_aShortKeyPos[0].x);
    const auto uPosY = reinterpret_cast<uintptr_t>(&g_aShortKeyPos[0].y);

    // CQuickslotKeyMappedMan: 26 keys, backup copy at +0x6C
    ATTACH_HOOK(CQuickslotKeyMappedMan__ctor, CQuickslotKeyMappedMan__ctor_hook);
    ATTACH_HOOK(CQuickslotKeyMappedMan__DefaultQuickslotKeyMap, CQuickslotKeyMappedMan__DefaultQuickslotKeyMap_hook);
    PatchJmp(0x009FA0DF, &QkmAlloc_hook);                  // sub_9FA0CB: push 44h + mov ecx (7 B) -> alloc 0xD4
    PatchNop(0x009FA0DF + 5, 0x009FA0E6);                  // ... rest of mov ecx
    Patch1(0x0072B83E + 1, kSlots * 4);                    // CQuickslotKeyMappedMan::OnInit DecodeBuffer 20h -> 68h
    Patch1(0x0072B861 + 1, kSlots * 4);                    // OnInit memcpy(backup, keys) size 20h -> 68h
    Patch1(0x0072B867 + 2, kBackupOffset);                 // OnInit lea eax,[esi+24h] -> +6Ch
    Patch1(0x0072B8A0 + 1, kSlots * 4);                    // SaveQuickslotKeyMap EncodeBuffer (CP 0xB7) 20h -> 68h
    Patch1(0x0072B8BD + 1, kSlots * 4);                    // SaveQuickslotKeyMap memcpy size 20h -> 68h
    Patch1(0x0072B8C0 + 2, kBackupOffset);                 // SaveQuickslotKeyMap add esi,24h -> 6Ch
    Patch1(0x00833791 + 1, kSlots * 4);                    // CUIKeyConfig::OnDestroy memcmp size 20h -> 68h
    Patch1(0x00833797 + 2, kBackupOffset);                 // CUIKeyConfig::OnDestroy lea eax,[edi+24h] -> +6Ch
    Patch1(0x0083383B + 1, kSlots * 4);                    // CUIKeyConfig::OnDestroy memcmp size 20h -> 68h
    Patch1(0x00833841 + 2, kBackupOffset);                 // CUIKeyConfig::OnDestroy lea eax,[edi+24h] -> +6Ch
    Patch1(0x0083287F + 2, kBackupOffset);                 // sub_832834 (CUIKeyConfig restore) lea ecx,[eax+24h] -> +6Ch
    Patch1(0x00832882 + 1, kSlots * 4);                    // sub_832834 memcpy size 20h -> 68h
    Patch1(0x00836A1E + 1, kSlots * 4);                    // sub_8369D0 (CUIKeyConfig) memcmp size 20h -> 68h
    Patch1(0x00836A21 + 2, kBackupOffset);                 // sub_8369D0 add eax,24h -> 6Ch

    // CUIStatusBar per-slot cache -> g_abQsCache
    ATTACH_HOOK(CUIStatusBar__ctor, CUIStatusBar__ctor_hook);
    PatchMovImm(0x008DD898, 0xB8, uCache, 6);              // CompareValidateFuncKeyMappedInfo lea eax,[esi+0D20h] -> mov eax,cache
    Patch1(0x008DD8AB + 2, kSlots);                        // CompareValidateFuncKeyMappedInfo cmp edx,8 -> 26
    PatchJmp(0x008DD8B8, &CacheMemset_hook);               // CompareValidateFuncKeyMappedInfo memset size 60h -> 138h
    PatchMovImm(0x008DD8FD, 0xBB, uCache, 6);              // CompareValidateFuncKeyMappedInfo lea ebx,[esi+0D20h] -> mov ebx,cache
    Patch4(0x008DD913 + 3, kSlots);                        // CompareValidateFuncKeyMappedInfo mov [ebp-18h],8 -> 26
    PatchMovImm(0x008DDF99, 0xB8, uCache, 8);              // CQuickSlot::Draw mov eax,[ebp-3Ch]; add eax,0D20h -> mov eax,cache
    Patch1(0x008DE75E + 3, 4 + kSlots * 4);                // CQuickSlot::Draw loop cmp [ebp-48h],24h -> 6Ch
    Patch1(0x008D7F1E + 1, 0x34);                          // CUIStatusBar::OnMouseMove lea esi,[ebx+eax*4+0D1Ch] (ebx = this+4)
    Patch1(0x008D7F1E + 2, 0x85);                          // ... -> lea esi,[eax*4+disp32]
    Patch4(0x008D7F1E + 3, static_cast<unsigned int>(uCache)); // ... = cache

    // CUIStatusBar per-slot cooldown index -> g_anQsCooltime (tempstat.cpp reads it through [ebp-18h])
    PatchMovImm(0x008E069D, 0xBE, reinterpret_cast<uintptr_t>(g_anQsCooltime), 6); // DrawSkillCooltime lea esi,[ebx+0DC4h] -> mov esi,cooltime
    PatchMovImm(0x008E06A3, 0xBF, uCache + 1, 6);          // DrawSkillCooltime lea edi,[ebx+0D21h] -> mov edi,cache+1
    Patch1(0x008E099F + 3, kSlots);                        // DrawSkillCooltime cmp [ebp-1Ch],8 -> 26

    // Slot positions: s_ptShortKeyPos 0x00BE2DB0 -> g_aShortKeyPos
    Patch1(0x008DE941 + 2, kSlots);                        // CQuickSlot::GetPosByIndex cmp eax,8 -> 26
    Patch4(0x008DE94D + 2, static_cast<unsigned int>(uPosX)); // CQuickSlot::GetPosByIndex mov ecx,[eax+0BE2DB0h]
    Patch4(0x008DE955 + 2, static_cast<unsigned int>(uPosY)); // CQuickSlot::GetPosByIndex mov eax,[eax+0BE2DB4h]
    Patch4(0x008DE8F4 + 1, static_cast<unsigned int>(uPosY)); // CUIStatusBar::GetShortCutIndexByPos mov esi,0BE2DB4h
    Patch4(0x008DE926 + 2, static_cast<unsigned int>(reinterpret_cast<uintptr_t>(&g_aShortKeyPos[kSlots].y))); // ... loop end cmp esi,0BE2DF4h

    // Bar size and position (800x600 frame; resolution.cpp moves the ms_pOrgQuickSlot origin)
    Patch4(0x008D155C + 1, kBarWidth);                     // CUIStatusBar::OnCreate quickslot back layer width 97h
    Patch4(0x008D182E + 1, kBarWidth);                     // CUIStatusBar::OnCreate quickslot icon layer width 97h
    Patch4(0x008D1AC0 + 1, kBarWidth);                     // CUIStatusBar::OnCreate quickslot cooltime layer width 97h
    Patch4(0x008D179A + 1, kBarX);                         // CUIStatusBar::OnCreate quickslot layer x 287h
    Patch4(0x008DF7F8 + 1, kBarX);                         // CUIStatusBar::ToggleQuickSlot layer x 287h
    Patch4(0x008DE8E5 + 2, static_cast<unsigned int>(-kBarX)); // CUIStatusBar::GetShortCutIndexByPos lea edi,[eax-287h]
    Patch4(0x008DE896 + 1, kBarX);                         // CUIStatusBar::HitTest mov ecx,287h: quickslot left edge

    // Other 8-slot bounds
    Patch1(0x004F928A + 2, kSlots);                        // CDraggableMenu::OnDropped cmp eax,8 -> 26
    Patch1(0x004F93F9 + 2, kSlots);                        // CDraggableMenu::MapFuncKey cmp eax,8 -> 26
    Patch1(0x004F953D + 3, kSlots);                        // sub_4F94F3 (CDraggableMenu, already on a quickslot?) cmp [ebp-4],8 -> 26
}
