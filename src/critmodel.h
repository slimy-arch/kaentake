#pragma once
namespace CritModel {

// The base every character has before any skill contributes.
constexpr int kBaseCritRate   = 10;    // percent
constexpr int kBaseCritDamage = 150;   // percent of a normal hit (150 = 1.5x)

// PDamage's gate needs a nonzero "skill level" for the roll to happen at all,
// so the resolver hook reports this when the job has no crit passive learned.
constexpr int kBaseSkillLevel = 1;

// A wz crit-damage field (authored as a multiplier percent) -> the bonus it
// contributes over the 100% baseline the client's own math assumes.
constexpr int BonusFromMultiplier(int nAuthored) {
    return nAuthored > 100 ? nAuthored - 100 : 0;
}

} // namespace CritModel

// True once critrouting.cpp's Sharp Eyes crit-damage cave is installed. The
// display reads this so it can fall back to the client's raw add and never
// show a number combat is not using. Defined in critrouting.cpp.
bool CritModel_IsSharpEyesNormalized();

// Steady-state crit numbers for UI (250ms cache). Defined in critratedisplay.cpp.
int CritDisplay_GetCritRate();
int CritDisplay_GetCritDamage();

// True for the magician family ((job % 1000) / 100 == 2). Spells go through MDamage,
// which never rolls a crit, so the stat window shows "-" instead of a rate they
// cannot use. Same 250ms cache. Defined in critratedisplay.cpp.
bool CritDisplay_IsMagician();
