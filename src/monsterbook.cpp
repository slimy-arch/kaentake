#include "pch.h"
#include "constants.h"
#include "hook.h"

// ===========================================================================
// CUIMonsterBook -- open the Monster Book up.
//
// Stock v83 rations the book out as the player collects cards: un-owned cards
// are drawn greyed and translucent with no counter, the four right-hand tabs
// unlock one at a time, and the Basic Info page hides the mob art and prints
// "HP : ???" until enough cards are in. Mush wants the whole book legible from
// the first login -- every icon in colour, every card carrying a count that
// starts at 0, and all four tabs always readable.
//
// The gates are five spots in the exe, all of them a single branch, so this is
// five surgical writes plus one detour rather than a re-implementation.
//
//   CUIMonsterBook::Draw  -- card grid ----------------------------------
//   0x00864959  jne 0x00864C23   after sub_95FC65(cardId) (card level, 0 = not
//                                owned). Taken  -> full-colour icon, alpha 0xFF.
//                                Not taken -> sub_98DD63 greyscale + alpha 0x55.
//                                => forced unconditional: never grey, never faded.
//   0x00864E5A  je  0x0086509A   "level == 0, skip the counter". NOP'd, so the
//                                UI/Basic.img/ItemNo digits also render the 0.
//                                (5/5 still takes the fullMark branch above it,
//                                 guarded by its own sub_95FCB7 test -- untouched.)
//
//   CUIMonsterBook::Draw  -- Basic Info page -----------------------------
//   0x00865F9C  jle 0x0086651C   "level <= 0, don't draw the mob art". NOP'd.
//   0x00866551  jle 0x00866581   "level <= 1, keep HP : ??? / MP : ???". NOP'd,
//                                so the real numbers print. They come from
//                                CMonsterBookMan's record, which LoadBook fills
//                                from Mob/%07d.img info/maxHP (or info/defaultHP)
//                                -- ONLY for mob ids present in
//                                String/MonsterBook.img. A mob missing there has
//                                no record at all, GetInfo returns NULL, the
//                                setter bails at 0x0086721A and the previous
//                                mob's strings stay put -> "HP : (null)". That
//                                half is a DATA fix (Cosmic tools/monsterbook_gen
//                                writes an entry for all 860 card mobs); without
//                                it this NOP would just print (null) sooner.
//
//   CUIMonsterBook::SetTabEnable(int nCardLevel) @ 0x00866B2D -------------
//   Enables tab i by comparing the level: 0 (Basic Info) > 0, 1 (Episode) > 2,
//   2 (Dropping) > 3, 3 (Found In) > 4, and always disables tab 4. Detoured to
//   call the ORIGINAL with a level of 5 -- that satisfies all four comparisons
//   and keeps tab 4 off, so no calling convention is re-derived here.
// ===========================================================================

namespace {

constexpr uintptr_t kCardIconOwnedBranch = 0x00864959; // jne  -> full-colour icon path
constexpr uintptr_t kCardCountSkip       = 0x00864E5A; // je   -> skip the counter digits
constexpr uintptr_t kBasicInfoArtGate    = 0x00865F9C; // jle  -> skip the mob art
constexpr uintptr_t kBasicInfoHpMpGate   = 0x00866551; // jle  -> keep "HP : ???"

class CUIMonsterBook {
public:
    MEMBER_HOOK(void, 0x00866B2D, SetTabEnable, int nCardLevel)
};

// Every tab this UI has art for. 5 clears all four "level > N" tests in the
// original (N = 0, 2, 3, 4) while leaving the unused fifth tab disabled.
constexpr int kUnlockAllTabs = 5;

// ---------------------------------------------------------------------------
// Card counter: the login snapshot and the live pickup disagree about the key.
//
// The book's grid asks for a level with the FULL card id -- CUIMonsterBook::Draw
// pulls it out of the card record CMonsterBookMan::LoadCardList built from the
// Item/Consume/0238.img node name (0x00684A31 atoi -> record+0), then calls
// GetCardLevel at 0x0086494E.
//
// MONSTER_BOOK_SET_CARD (a pickup) carries that same full id as an int, so cards
// collected in this session answer correctly. The CHARSTATS login snapshot does
// not: it writes `cardid % 10000` into a short, so every card the character
// already owned is filed under 3024 while the grid looks up 2383024 -- the
// lookup misses and the level reads 0. Stock v83 hid the "0" (the counter draw
// was gated on level != 0, the branch NOP'd above), which is why this only
// became visible once every card started showing a number.
//
// Fixed here rather than on the wire because the snapshot field is a short and
// cannot carry the full id. Taking the best of both keys is correct whichever
// convention the client used to file the entry, and the two key spaces cannot
// collide: card ids run 2380000..2388999, so `% 10000` is injective over them.
// ---------------------------------------------------------------------------
using CardLevelFn = int(__cdecl*)(int nCardId);

auto GetCardLevel_Orig = reinterpret_cast<CardLevelFn>(0x0095FC65);
auto IsCardComplete_Orig = reinterpret_cast<CardLevelFn>(0x0095FCB7);

constexpr int kCardIdBase = 2380000;

int __cdecl GetCardLevel_hook(int nCardId) {
    int nLevel = GetCardLevel_Orig(nCardId);
    if (nLevel <= 0 && nCardId >= kCardIdBase) {
        nLevel = GetCardLevel_Orig(nCardId % 10000);
    }
    return nLevel;
}

int __cdecl IsCardComplete_hook(int nCardId) {
    int bComplete = IsCardComplete_Orig(nCardId);
    if (!bComplete && nCardId >= kCardIdBase) {
        bComplete = IsCardComplete_Orig(nCardId % 10000);
    }
    return bComplete;
}

} // namespace

// ---------------------------------------------------------------------------
// Shared flag between the search module and the drop-% module.
//
// Both paint the RIGHT page: the item search renders plain item icons there with
// NO percentages, while the Dropping tab renders the same icons WITH them. They
// must never paint at once. The flag lives HERE, in the base module, rather than
// in either of them, so neither has to link against the other and the two can be
// built in any order.
// ---------------------------------------------------------------------------
namespace {
bool g_itemResultView = false;
}

void MonsterBookSearch_SetItemResultView(bool bOn) {
    g_itemResultView = bOn;
}

bool MonsterBookSearch_IsItemResultView() {
    return g_itemResultView;
}

void CUIMonsterBook::SetTabEnable_hook(int /*nCardLevel*/) {
    MUSH_FEATURE("monsterBook:SetTabEnable");
    CUIMonsterBook::SetTabEnable(this, kUnlockAllTabs);
}

void AttachMonsterBookMod() {
#if USE_MONSTER_BOOK_OPEN
    // jne rel32 -> jmp rel32 + one NOP. Target is unchanged (0x00864C23); only the
    // condition goes away, so the rel32 shifts by the one byte the opcode loses.
    Patch1(kCardIconOwnedBranch, 0xE9);
    Patch4(kCardIconOwnedBranch + 1, 0x00864C23 - (kCardIconOwnedBranch + 5));
    Patch1(kCardIconOwnedBranch + 5, 0x90);

    PatchNop(kCardCountSkip, kCardCountSkip + 6);       // je  rel32
    PatchNop(kBasicInfoArtGate, kBasicInfoArtGate + 6); // jle rel32
    PatchNop(kBasicInfoHpMpGate, kBasicInfoHpMpGate + 2); // jle rel8

    ATTACH_HOOK(CUIMonsterBook::SetTabEnable, CUIMonsterBook::SetTabEnable_hook);

    // must follow the counter patch above: it is what makes a wrong level visible
    ATTACH_HOOK(GetCardLevel_Orig, GetCardLevel_hook);
    ATTACH_HOOK(IsCardComplete_Orig, IsCardComplete_hook);
#endif
}
