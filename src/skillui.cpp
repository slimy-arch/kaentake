#include "pch.h"
#include "hook.h"


// Skill window (CUISkill): 4 -> 6 visible skill rows. Needs the 175x370 UI/UIWindow.img/Skill/backgrnd.
// Rows are 0x28 (40) px apart, so every row-bound constant grows by 2 * 0x28 = 0x50.

// CUISkill keeps its level-up buttons as ZRef<CCtrlButton>[4] at this+0x5C4, directly followed by the
// macro button ZRef at +0x5E4, so the array cannot grow in place. OnCreate and SetButton are redirected
// to this array instead. CUISkill is a singleton (ctor writes 0x00BF1080), so one array is enough.
struct ZRefRaw {
    void* pad;
    void* p;
};
static ZRefRaw g_aSkillUpBtn[6]; // POD on purpose: no static destructor releasing into a dead client at exit

static auto ZRef_CCtrlButton__ReleaseRaw = reinterpret_cast<void(__thiscall*)(ZRefRaw*)>(0x00428925); // ZRef<CCtrlButton>::_ReleaseRaw — null-safe, zeroes p

// CUISkill::~CUISkill body, entered with ecx = CUISkill+8. The stock dtor releases the in-object array
// here; release the relocated one too, before the CWnd base is torn down.
static auto CUISkill__dtor = reinterpret_cast<void(__thiscall*)(void*)>(0x008AA8FD);

void __fastcall CUISkill__dtor_hook(void* pThis, void* _EDX) {
    for (ZRefRaw& ref : g_aSkillUpBtn) {
        ZRef_CCtrlButton__ReleaseRaw(&ref);
    }
    CUISkill__dtor(pThis);
}


void AttachSkillUiMod() {
    ATTACH_HOOK(CUISkill__dtor, CUISkill__dtor_hook); // CUISkill::~CUISkill — v83 VA 0x008AA8FD

    // Layout
    Patch4(0x008AA86F + 1, 0x172); // CUISkill::CUISkill (0x008AA5AD) CreateWnd push 121h -> 172h: window height = backgrnd 370
    Patch4(0x008AACD5 + 1, 0xF0);  // CUISkill::OnCreate (0x008AAA88) scrollbar 0x7D1 height 9Bh -> F0h (6 rows)
    Patch4(0x008AAE23 + 1, 0x159); // CUISkill::OnCreate macro button 0x7E7 y 109h -> 159h
    Patch4(0x008AC4DF + 1, 0x15B); // CUISkill::Draw (0x008AC38A) SP number y 109h -> 15Bh

    // Row loops
    Patch4(0x008ACE76 + 3, 0x166); // CUISkill::Draw row loop cmp y,116h -> 166h: draw 6 rows
    Patch4(0x008ACD98 + 3, 0x13E); // CUISkill::Draw cmp y,0EEh -> 13Eh: separator under every row but the last visible one
    Patch4(0x008AD9F2 + 2, 0x14F); // CUISkill::GetSkillIndexFromPoint (0x008AD946) cmp esi,103h -> 14Fh: hit-test 6 rows
    Patch1(0x008AD7B4 + 2, 0xFB);  // CUISkill::SetScrollBar (0x008AD790) add eax,-3 -> -5: scroll range = count - 5
    Patch1(0x008AD7F8 + 2, 6);     // CUISkill::SetButtons (0x008AD7C9) no-skill loop cmp ebx,4 -> 6
    Patch1(0x008AD903 + 2, 6);     // CUISkill::SetButtons skill loop cmp ebx,4 -> 6

    // Level-up buttons
    Patch4(0x008AADAC + 3, 0x167); // CUISkill::OnCreate level-up button loop cmp y,117h -> 167h: create 6 (ids 0x7DA..0x7DF)
    Patch4(0x008AB929 + 2, 0x7E0); // CUISkill::OnButtonClicked (0x008AB919) cmp esi,7DEh -> 7E0h: ids 0x7DE/0x7DF level up rows 5/6
    Patch1(0x008AAD3C + 1, 0x05);  // CUISkill::OnCreate lea eax,[ebx+5C4h] -> lea eax,[disp32]
    Patch4(0x008AAD3C + 2, reinterpret_cast<uintptr_t>(g_aSkillUpBtn)); // ... = g_aSkillUpBtn
    Patch1(0x008AD920 + 1, 0x34);  // CUISkill::SetButton (0x008AD917) lea esi,[ecx+eax*8+5C4h] -> lea esi,[eax*8+disp32]
    Patch1(0x008AD920 + 2, 0xC5);  // ... SIB: scale 8, index eax, no base
    Patch4(0x008AD920 + 3, reinterpret_cast<uintptr_t>(g_aSkillUpBtn)); // ... = g_aSkillUpBtn
}
