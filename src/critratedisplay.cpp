#include "pch.h"
#include "hook.h"
#include "critmodel.h"

namespace {

// --- singletons / functions (all byte-verified against MapleStory/MapleStory.exe) ---
constexpr uintptr_t kAddr_CWvsContextSlot = 0x00BE7918;  // CWvsContext* global
constexpr uintptr_t kAddr_CUserLocalSlot  = 0x00BEBF98;  // CUserLocal* global

// CWvsContext::GetCharacterData — __thiscall(this, ZRef* out); raw ptr at out+4.
struct ZRefCD { void* unused; void* pCharacterData; };
auto GetCharacterData = reinterpret_cast<ZRefCD*(__thiscall*)(void*, ZRefCD*)>(0x00425D0B);
// ZRef<CharacterData> release — __thiscall(&zref); the call the client makes at
// 0x008CD8D2 immediately after taking the raw pointer.
auto ReleaseCharacterDataRef = reinterpret_cast<void(__thiscall*)(ZRefCD*)>(0x004289B7);

// get_critical_skill_level — v83 VA 0x00765095 (PDamage's; Detoured by critrouting.cpp).
auto GetWeaponCritSkillProp =
    reinterpret_cast<int(__cdecl*)(void*, int, int, int*, int*)>(0x00765095);

// ZtlSecureFuse_long __cdecl(ptr, checksum) — same fuse PDamage uses for Sharp
// Eyes. Detours-hooked (pass-through) by magicdmg.cpp; calling through is fine.
auto ZtlSecureFuse_long = reinterpret_cast<int(__cdecl*)(const void*, int)>(0x00416563);
// ZtlSecureFuse<short> — v83 VA 0x004746DD; the job at CharacterData+0x39 (checksum +0x3D).
auto ZtlSecureFuse_short = reinterpret_cast<unsigned int(__cdecl*)(const void*, int)>(0x004746DD);
constexpr int kMagicianJobClass = 2;   // (job % 1000) / 100, as in magicdmg.cpp

// Equipped weapon item id — CUserLocal+0x4EC (plain int; read at 0x00951DC7 on
// the way into the PDamage call).
constexpr uintptr_t kOff_WeaponItemID = 0x4EC;

// Sharp Eyes packed value — CWvsContext+0x2134 (SecondaryStat) + 0x5DC/+0x5E4.
constexpr uintptr_t kOff_SharpEyes         = 0x2710;
constexpr uintptr_t kOff_SharpEyesChecksum = 0x2718;

// nAttackType arg for the resolver: PDamage's melee caller passes 0 (0x00951E23),
// shoot passes 1 (0x00954CBD). The bow/crossbow/claw passives only apply to shoot
// attacks, which is how those jobs actually fight, so the window quotes that case.
constexpr int kAttackTypeShoot = 1;

// The full computation — the steady-state half of what PDamage would roll.
// POD locals and out-params only (__try forbids unwindables). On any failure
// both values fall back to the base: that is what a character with no skills
// has, and it is never misleading the way a 0 would be.
void QueryCrit(int* pnRate, int* pnDamage, bool* pbMagician) {
    *pnRate = CritModel::kBaseCritRate;
    *pnDamage = CritModel::kBaseCritDamage;
    *pbMagician = false;

    char* pCtx  = *reinterpret_cast<char**>(kAddr_CWvsContextSlot);
    char* pUser = *reinterpret_cast<char**>(kAddr_CUserLocalSlot);
    if (pCtx == nullptr || pUser == nullptr) {
        return;
    }
    int nRate = CritModel::kBaseCritRate;
    int nCritDamage = CritModel::kBaseCritDamage;
    __try {
        ZRefCD ref = {};
        GetCharacterData(pCtx, &ref);
        void* pCd = ref.pCharacterData;
        ReleaseCharacterDataRef(&ref);
        if (pCd == nullptr) {
            return;
        }
        const char* pCdBytes = static_cast<const char*>(pCd);
        const int nJob = static_cast<short>(ZtlSecureFuse_short(
            pCdBytes + 0x39, *reinterpret_cast<const int*>(pCdBytes + 0x3D)));
        *pbMagician = (nJob % 1000) / 100 == kMagicianJobClass;
        // The resolver hook seeds the base and adds the weapon's passive to both.
        const int nWeaponItemID = *reinterpret_cast<int*>(pUser + kOff_WeaponItemID);
        GetWeaponCritSkillProp(pCd, nWeaponItemID, kAttackTypeShoot, &nRate, &nCritDamage);

        // Sharp Eyes, mirroring PDamage 0x0078E160..0x0078E1CD as normalized by
        // critrouting.cpp: rate is the high byte clamped 0..100, crit damage is
        // the low byte's bonus over 100.
        const int nFused = ZtlSecureFuse_long(
            pCtx + kOff_SharpEyes,
            *reinterpret_cast<int*>(pCtx + kOff_SharpEyesChecksum));
        if (nFused > 0) {
            int nSharpEyesRate = nFused >> 8;
            if (nSharpEyesRate < 0) {
                nSharpEyesRate = 0;
            } else if (nSharpEyesRate > 100) {
                nSharpEyesRate = 100;
            }
            nRate += nSharpEyesRate;
            // Mirror whatever PDamage is actually doing: normally the cave has
            // turned its add into a bonus, but if that guard missed, combat is
            // still adding the raw multiplier and so must the display.
            nCritDamage += CritModel_IsSharpEyesNormalized()
                               ? CritModel::BonusFromMultiplier(nFused & 0xFF)
                               : (nFused & 0xFF);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    // Rate is a probability: anything at or above 100 crits every hit. Crit
    // damage has no cap in the client and is shown as-is.
    if (nRate < 0) {
        nRate = 0;
    } else if (nRate > 100) {
        nRate = 100;
    }
    *pnRate = nRate;
    *pnDamage = nCritDamage;
}

// Throttled: the row redraws every frame while the detail panel is open, and a
// fresh skill query per frame is pointless. 250ms keeps a Sharp Eyes cast
// visibly instant.
int   g_nCritRate   = CritModel::kBaseCritRate;
int   g_nCritDamage = CritModel::kBaseCritDamage;
bool  g_bMagician   = false;
DWORD g_tLastQuery  = 0;

void RefreshCrit() {
    const DWORD tNow = GetTickCount();
    if (tNow - g_tLastQuery > 250 || g_tLastQuery == 0) {  // unsigned sub is wrap-safe
        g_tLastQuery = tNow;
        QueryCrit(&g_nCritRate, &g_nCritDamage, &g_bMagician);
    }
}

} // namespace

// ============================================================
// PUBLIC READERS — the numbers any UI surface should show.
//
// Both are the steady-state values (what a normal hit rolls against), share
// the 250ms cache, and are safe to call from any in-game main-thread draw
// path. CritDisplay_GetCritDamage() has no draw site of its own in the client;
// statdetaillayout.cpp draws it into the detail window's CRITICAL DAM. cell.
// ============================================================
int CritDisplay_GetCritRate() {
    RefreshCrit();
    return g_nCritRate;
}

int CritDisplay_GetCritDamage() {
    RefreshCrit();
    return g_nCritDamage;
}

bool CritDisplay_IsMagician() {
    RefreshCrit();
    return g_bMagician;
}
