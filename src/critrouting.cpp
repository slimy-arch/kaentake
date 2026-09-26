#include "pch.h"
#include "hook.h"
#include "critmodel.h"

#include <cstring>

// ============================================================
// CRIT MODEL — base crit for every character + the vanilla passives.
//
// get_critical_skill_level — v95 sym, v83 VA 0x00765095 (unnamed in v83.idb; its only
// caller is CalcDamage::PDamage @0x0078E112). __cdecl(CharacterData*, nWeaponItemID,
// nAttackType, int* pnProp, int* pnDamage), returns the passive's level.
//
// The stock body (0x00765095-0x00765236) picks ONE passive from the weapon type:
//   bow / crossbow (45, 46), shoot attacks only -> 3000001 Critical Shot
//                                                  (13000000 for Cygnus, job/1000 == 1)
//   claw (47), shoot attacks only               -> 4100001 Critical Throw
//                                                  (14100001 for Cygnus)
//   knuckle (48), any attack                    -> 15110000 Critical Punch
//   anything else                               -> nothing, both outs 0
// and writes the passive's `prop` and `damage` straight into the outs.
//
// The hook keeps that exact selection and adds two things on top (critmodel.h):
//   * every character starts at kBaseCritRate / kBaseCritDamage, so physical attacks
//     can crit without a passive;
//   * the passive's `damage` (a multiplier percent) adds only its bonus over 100.
// Magic never reaches this function — MDamage does not call it — so spells do not crit.
// ============================================================
static auto CritPassiveResolver = 0x00765095;

namespace {

constexpr uintptr_t kAddr_CSkillInfoSlot = 0x00BE78DC;  // CSkillInfo* singleton (read @0x007650D0)

// get_weapon_type — v83 VA 0x00460AA0, __cdecl(nItemID); the call the stock resolver makes
// at 0x007650AA. Returns (id / 10000) % 100 for weapons, 0 otherwise.
auto GetWeaponType = reinterpret_cast<int(__cdecl*)(int)>(0x00460AA0);
// ZtlSecureFuse<short> — v83 VA 0x004746DD. Detoured by maxhpmp.cpp, whose hook only
// diverts checksum 0 (its HP/MP FakeTear sentinel); the job read below passes through.
auto ZtlSecureFuse_short = reinterpret_cast<unsigned int(__cdecl*)(const void*, int)>(0x004746DD);
// ZtlSecureFuse<long> — v83 VA 0x00416563 (Detoured pass-through by magicdmg.cpp).
auto ZtlSecureFuse_long = reinterpret_cast<int(__cdecl*)(const void*, int)>(0x00416563);
// CSkillInfo::GetSkillLevel — v83 VA 0x007616F6, __thiscall, ret 0xC.
auto GetSkillLevel = reinterpret_cast<int(__thiscall*)(void*, void*, int, void**)>(0x007616F6);
// SKILLENTRY::GetLevelData — v83 VA 0x00760F23, __thiscall(SKILLENTRY*, nLevel).
auto GetSkillLevelData = reinterpret_cast<char*(__thiscall*)(void*, int)>(0x00760F23);

constexpr int kWeaponBow      = 45;
constexpr int kWeaponCrossbow = 46;
constexpr int kWeaponClaw     = 47;
constexpr int kWeaponKnuckle  = 48;
constexpr int kAttackShoot    = 1;   // PDamage's shoot caller passes 1 (0x00954CBD)

// Job from CharacterData, exactly as the stock claw/bow arms read it (0x00765102):
// ZtlSecure<short> at +0x39, checksum at +0x3D.
int JobFromCharacterData(const void* pCd) {
    const char* p = static_cast<const char*>(pCd);
    return static_cast<short>(ZtlSecureFuse_short(p + 0x39, *reinterpret_cast<const int*>(p + 0x3D)));
}

// The stock selection, verbatim. 0 = this weapon/attack has no crit passive.
int CritPassiveFor(const void* pCd, int nWeaponItemID, int nAttackType) {
    const int nType = GetWeaponType(nWeaponItemID);
    if (nType == kWeaponKnuckle) {
        return 15110000;                                        // Critical Punch
    }
    if (nAttackType != kAttackShoot) {
        return 0;
    }
    const bool bCygnus = JobFromCharacterData(pCd) / 1000 == 1;
    if (nType == kWeaponBow || nType == kWeaponCrossbow) {
        return bCygnus ? 13000000 : 3000001;                    // Critical Shot
    }
    if (nType == kWeaponClaw) {
        return bCygnus ? 14100001 : 4100001;                    // Critical Throw
    }
    return 0;
}

int __cdecl CritPassiveResolver_hook(void* pCd, int nWeaponItemID, int nAttackType,
                                     int* pnCritRate, int* pnCritDamage) {
    // PDamage's gate is rate > 0 AND damage > 0 AND (this return > 0 OR Sharp Eyes OR
    // Chance Attack OR the Aran combo flag). Returning kBaseSkillLevel when no passive
    // applies is what lets the base crit roll for everyone.
    *pnCritRate = CritModel::kBaseCritRate;
    *pnCritDamage = CritModel::kBaseCritDamage;
    if (pCd == nullptr) {
        return CritModel::kBaseSkillLevel;
    }
    const int nSkillId = CritPassiveFor(pCd, nWeaponItemID, nAttackType);
    void* pSkillInfo = *reinterpret_cast<void**>(kAddr_CSkillInfoSlot);
    if (nSkillId == 0 || pSkillInfo == nullptr) {
        return CritModel::kBaseSkillLevel;
    }
    void* pEntry = nullptr;
    const int nLevel = GetSkillLevel(pSkillInfo, pCd, nSkillId, &pEntry);
    if (nLevel <= 0 || pEntry == nullptr) {
        return CritModel::kBaseSkillLevel;                      // passive not learned
    }
    char* pLevelData = GetSkillLevelData(pEntry, nLevel);
    if (pLevelData == nullptr) {
        return CritModel::kBaseSkillLevel;
    }
    // Same two SKILLLEVELDATA fields the stock body fuses (0x007651E0 / 0x00765204).
    *pnCritRate += ZtlSecureFuse_long(pLevelData + 0xF4, *reinterpret_cast<int*>(pLevelData + 0xFC));
    *pnCritDamage += CritModel::BonusFromMultiplier(
        ZtlSecureFuse_long(pLevelData + 0xD0, *reinterpret_cast<int*>(pLevelData + 0xD8)));
    return nLevel;
}

// ============================================================
// PDamage's own crit-damage adds, normalized to the same "bonus over 100" convention.
// Both caves are pure arithmetic on EAX: no calls, flags overwritten by the next insn.
// ============================================================

// Sharp Eyes, "crit damage already nonzero" path (always, given the base):
//   0x0078E1C5  and eax, 0FFh        <- 5 bytes, ours
//   0x0078E1CA  add [ebp-34h], eax
constexpr uintptr_t kAddr_SharpEyesAdd = 0x0078E1C5;  // CalcDamage::PDamage+0x23E
DWORD kSharpEyesAdd_Ret = 0x0078E1CA;

__declspec(naked) void SharpEyesCritDamageCave() {
    __asm {
        and     eax, 0FFh               // re-run overwritten: packed low byte
        sub     eax, 100                // multiplier percent -> bonus
        jge     se_keep
        xor     eax, eax
    se_keep:
        jmp     [kSharpEyesAdd_Ret]     // -> 0x0078E1CA
    }
}

// Sharp Eyes, "crit damage still zero" path: stock computes 0 + y + 100 with
// `lea eax, [ecx+eax+64h]`; the displacement becomes (base - 100) so the total is
// base + bonus like every other source.
constexpr uintptr_t kAddr_SharpEyesLea = 0x0078E1B6;  // CalcDamage::PDamage+0x22F
static_assert(CritModel::kBaseCritDamage - 100 >= 0 && CritModel::kBaseCritDamage - 100 <= 127,
              "base crit damage must fit the lea's signed byte displacement");

// Chance Attack (vs stunned mobs):
//   0x0078E24F  add [ebp-34h], eax ; pop ecx ; pop ecx   <- 5 bytes, ours
constexpr uintptr_t kAddr_ChanceAtkAdd = 0x0078E24F;  // CalcDamage::PDamage+0x2C8
DWORD kChanceAtkAdd_Ret = 0x0078E254;

__declspec(naked) void ChanceAttackCritDamageCave() {
    __asm {
        sub     eax, 100
        jge     ca_keep
        xor     eax, eax
    ca_keep:
        add     dword ptr [ebp-34h], eax   // re-run overwritten
        pop     ecx
        pop     ecx
        jmp     [kChanceAtkAdd_Ret]        // -> 0x0078E254
    }
}

bool g_bSharpEyesNormalized = false;

void InstallCritDamageNormalization() {
    static const unsigned char kSharpEyesSite[] = { 0x25, 0xFF, 0x00, 0x00, 0x00 };
    if (memcmp(reinterpret_cast<void*>(kAddr_SharpEyesAdd), kSharpEyesSite, sizeof(kSharpEyesSite)) == 0) {
        PatchJmp(kAddr_SharpEyesAdd, reinterpret_cast<uintptr_t>(&SharpEyesCritDamageCave)); // PDamage — Sharp Eyes crit damage
        g_bSharpEyesNormalized = true;
    } else {
        ErrorMessage("Crit model: Sharp Eyes crit-damage add at 0x%08X does not match - skipping.", kAddr_SharpEyesAdd);
    }

    static const unsigned char kSharpEyesLeaSite[] = { 0x8D, 0x44, 0x01, 0x64 };
    if (memcmp(reinterpret_cast<void*>(kAddr_SharpEyesLea), kSharpEyesLeaSite, sizeof(kSharpEyesLeaSite)) == 0) {
        Patch1(kAddr_SharpEyesLea + 3, static_cast<unsigned char>(CritModel::kBaseCritDamage - 100)); // PDamage — Sharp Eyes, zero-damage path
    } else {
        ErrorMessage("Crit model: Sharp Eyes no-skill path at 0x%08X does not match - skipping.", kAddr_SharpEyesLea);
    }

    static const unsigned char kChanceAtkSite[] = { 0x01, 0x45, 0xCC, 0x59, 0x59 };
    if (memcmp(reinterpret_cast<void*>(kAddr_ChanceAtkAdd), kChanceAtkSite, sizeof(kChanceAtkSite)) == 0) {
        PatchJmp(kAddr_ChanceAtkAdd, reinterpret_cast<uintptr_t>(&ChanceAttackCritDamageCave)); // PDamage — Chance Attack crit damage
    } else {
        ErrorMessage("Crit model: Chance Attack crit-damage add at 0x%08X does not match - skipping.", kAddr_ChanceAtkAdd);
    }
}

} // namespace

bool CritModel_IsSharpEyesNormalized() {
    return g_bSharpEyesNormalized;
}

void AttachCritRoutingMod() {
    static const unsigned char kResolverEntry[] = {
        0x55,                   // push ebp
        0x8B, 0xEC,             // mov  ebp, esp
        0x53,                   // push ebx
        0x8B, 0x5D, 0x14,       // mov  ebx, [ebp+0x14]  (pnCritRate)
        0x56,                   // push esi
        0x8B, 0x75, 0x18,       // mov  esi, [ebp+0x18]  (pnCritDamage)
    };
    if (memcmp(reinterpret_cast<void*>(CritPassiveResolver), kResolverEntry, sizeof(kResolverEntry)) != 0) {
        ErrorMessage("Crit routing: resolver entry at 0x%08X does not match - skipping.", CritPassiveResolver);
        return;
    }
    if (!ATTACH_HOOK(CritPassiveResolver, CritPassiveResolver_hook)) {
        return;
    }
    // Only once something seeds the base these normalize against.
    InstallCritDamageNormalization();
}
