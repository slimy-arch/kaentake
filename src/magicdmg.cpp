#include "pch.h"
#include "hook.h"
#include "magicdmg.h"

#include <intrin.h>
#include <cstring>
#include <cstdlib>
#include <random>

// ============================================================
// SHARED STATE
// Written by ztlSecureFuse_hook on every stat read,
// consumed by MagicFormula_cave on every damage event.
// ============================================================

static int    g_mad_int        = 0;   // INT stat (total)
static int    g_mad_magic      = 0;   // base magic attack
static int    g_mad_bonusmagic = 0;   // bonus magic attack (buffs / equips)
static double g_mad_topMAD     = 0.0; // computed damage (formula × variance)
static double g_mad_Hundred    = 100.0;

// Integers cannot hold the uncapped formula once INT/MAD are large: MSVC's
// double→int conversion of a value above 2^31-1 yields INT_MIN (0x80000000),
// and the old `result > 0 ? result : 1` then stored 1 — every mage hit for 1.
// 2^53 is the last integer a double can represent exactly.
static const double kExactIntLimit = 9007199254740992.0;

static long long DamageToLong(double d) {
    if (!(d > 0.0)) {
        return 0;
    }
    if (d >= kExactIntLimit) {
        return 9007199254740992LL;
    }
    return static_cast<long long>(d);
}

// The client's own mastery-derived low-end fraction, snapshotted out of MDamage's
// frame by the cave below. See MasteryFloor() for what is done with it.
static double g_mad_masteryFactor = 0.0;

// Map the client mastery factor onto the configured damage-floor band.

// The stock factor's domain for magic, from the WZ data above. kStockFactorLo is
// mastery 1, kStockFactorHi is mastery 10. A skill with no mastery node at all yields
// 0.09, which falls below kStockFactorLo and therefore takes kFloorMin - i.e. exactly
// the behaviour this file had before.
static const double kStockFactorLo = 0.135;
static const double kStockFactorHi = 0.540;

// The floor band. kFloorMin is deliberately the old hardcoded value so that no build
// of no character loses damage to this change.
static const double kFloorMin = 0.60;
static const double kFloorMax = 0.90;

// Written so that a NaN in dStockFactor falls through the first `!(x > lo)` test and
// returns kFloorMin, rather than propagating into the distribution and taking the
// whole attack path down.
static double MasteryFloor(double dStockFactor) {
    if (!(dStockFactor > kStockFactorLo)) {
        return kFloorMin;
    }
    if (dStockFactor >= kStockFactorHi) {
        return kFloorMax;
    }
    return kFloorMin + (kFloorMax - kFloorMin)
                     * (dStockFactor - kStockFactorLo) / (kStockFactorHi - kStockFactorLo);
}

// ============================================================
// FORMULA + VARIANCE
// Called from MagicFormula_cave before the x87 ASM block.
// __cdecl so MSVC generates a proper frame that preserves EBP;
// the naked cave's EBP (= calling function's frame pointer)
// is intact when the __asm block runs after this returns.
// ============================================================

// THE formula, in one place. Shared verbatim by combat (DoCalcMAD, below) and by the
// stat window's RANGE row (MagicDmg_GetRange), so the number on screen cannot drift
// from the number that lands on the monster. Returns a double and truncates NOWHERE:
// combat multiplies by its variance first and truncates once, and rounding earlier
// would change the damage this file has been shipping.
static double MaxMagicDamage(int nInt, int nTotalMAD) {
    const double dInt = static_cast<double>(nInt);
    const double dMad = static_cast<double>(nTotalMAD);
    return (dInt * 5.0 + dMad) * dMad / 100.0 + dInt;
}

static void __cdecl DoCalcMAD() {
    // New formula: (INT*5 + totalMAD) * totalMAD / 100 + INT
    double maxMAD = MaxMagicDamage(g_mad_int, g_mad_magic + g_mad_bonusmagic);

    // Continuous uniform variance in [MasteryFloor(...), 1.00].
    // Dedicated, locally-seeded generator: independent of the global
    // rand() stream and gives full double precision (vs. the coarse,
    // platform-dependent rand()/RAND_MAX granularity).
    //
    // The generator stays static (one stream for the session); the DISTRIBUTION
    // cannot, because the floor now moves per skill. Constructing it per call is
    // free - uniform_real_distribution holds nothing but its two bounds.
    static std::mt19937 s_varianceRng(std::random_device{}());
    std::uniform_real_distribution<double> varianceDist(MasteryFloor(g_mad_masteryFactor), 1.00);
    double variance = varianceDist(s_varianceRng);

    double result = maxMAD * variance;
    if (!(result > 0.0)) {
        result = 1.0;
    } else if (result > kExactIntLimit) {
        result = kExactIntLimit;
    }
    g_mad_topMAD = result;
}

// ============================================================
// CODE CAVE — installed at 0x00791C41 (9 bytes: JMP + 4 NOPs)
//
// On entry: EBP = calling function's frame pointer (untouched,
//           since the cave is reached via JMP not CALL).
//   [ebp + 0x30] = skill damage% (integer, same as MapleRoot)
//
// On exit:  x87 FPU ST(0) = topMAD × skillDmg% / 100
//           Execution continues at 0x00791C6C.
// ============================================================

static DWORD kMagicFormula_Return = 0x00791C6C;

__declspec(naked) static void MagicFormula_cave() {
    __asm {
        // Snapshot the client's own mastery-derived low end BEFORE anything else runs.
        // It lives in MDamage's frame at [ebp-0x94] (written @0x00791777, read by stock
        // @0x00791BE2) and EBP is still MDamage's here, because the cave is entered by
        // JMP and not by CALL.
        //
        // Every path into 0x00791C41 passes 0x00791BE2, which reads the same slot, so it
        // is always initialised: the three predecessors are the fall-through from
        // 0x00791C3F, `je 0x00791C41` at 0x00791C1B, and the 0x00791C15 arm - all of them
        // downstream of that read.
        //
        // BALANCED ON PURPOSE: one push, one pop, so the x87 stack depth handed to
        // 0x00791C6C is exactly what it was without this block. Getting that wrong
        // desyncs the FPU stack for the rest of the frame.
        fld  qword ptr [ebp - 0x94]
        fstp g_mad_masteryFactor
    }
    DoCalcMAD();   // computes g_mad_topMAD; preserves EBP (callee-saved)
    __asm {
        fld   g_mad_topMAD             // ST(0) = topMAD (double, not int32)
        fimul dword ptr [ebp + 0x30]   // ST(0) *= skillDmg%
        fdiv  g_mad_Hundred            // ST(0) /= 100
        jmp   [kMagicFormula_Return]   // → 0x00791C6C
    }
}

// ============================================================
// ztlSecureFuse HOOK
//
// Detours-hooks the client's stat-decrypt function (0x00416563).
// Each call site in the damage function reads a different stat;
// we identify which one by the return address left on the stack.
//
// Return addresses (each verified: the preceding insn is `call 0x00416563`):
//   0x00791BC9  INT  — main damage path
//   0x008C677D  INT  — tooltip / secondary path
//   0x00791650  magic (base MAD) — main damage path
//   0x008C36A4  magic (base MAD) — tooltip path
//   0x0079165E  bonusmagic (buff / equip MAD)
// ============================================================

static auto ztlSecureFuse_orig =
    reinterpret_cast<unsigned int(__cdecl*)(int, int)>(0x00416563);

static unsigned int __cdecl ztlSecureFuse_hook(int a1, int a2) {
    unsigned int result = ztlSecureFuse_orig(a1, a2);

    switch (reinterpret_cast<uintptr_t>(_ReturnAddress())) {
    case 0x00791BC9:  // INT — main damage path
    case 0x008C677D:  // INT — tooltip path
        g_mad_int = static_cast<int>(result);
        break;
    case 0x00791650:  // magic (base MAD) — main damage path
    case 0x008C36A4:  // magic (base MAD) — tooltip path
        g_mad_magic = static_cast<int>(result);
        break;
    case 0x0079165E:  // bonusmagic
        g_mad_bonusmagic = static_cast<int>(result);
        break;
    default:
        break;
    }

    return result;
}

// ============================================================
// STAT WINDOW PROVIDER - the RANGE row for magicians
//
// WHAT WAS ACTUALLY WRONG. v83 has no magic damage range at all. CUIStatDetail::Draw
// (0x008C2870) computes ONE range for every job: it picks a weapon multiplier out of
// the jump table at 0x008C452C by weapon type, and wands (37) and staves (38) share
// the 1H-axe arm. So a magician's RANGE row was a PHYSICAL range off their wand's PAD -
// a number with no relationship to what they actually hit for. It was not "the magic
// range computed by the stock formula"; there was nothing there to be stale.
//
// This provider gives the window the real thing, straight from the same expression
// combat runs (MaxMagicDamage above).
//
// EXACT PARITY WITH COMBAT, and the two places it is deliberately NOT what the client
// would compute:
//   * totalMAD is the RAW base + bonus. MDamage additionally adds an incoming
//     per-skill term ([ebp+0x40] @0x00791660), clamps to 1999 (@0x0079166C) and then
//     applies the % attack rate at +0x768 (@0x0079167D) - but DoCalcMAD sees NONE of
//     that, because ztlSecureFuse_hook captures the two raw fuses at return addresses
//     0x00791650 and 0x0079165E, upstream of all three. Reproducing base+bonus is
//     therefore what MATCHES; applying the rate here would over-report.
//   * The MIN uses kFloorMin, not the cast skill's mastery. Magic mastery in this tree
//     is authored per ATTACK SKILL (Fire Arrow, Ice Strike, ...), not per weapon, so
//     there is no such thing as "the character's magic mastery" for a window that does
//     not know what you are about to cast. kFloorMin is the honest floor: it is the
//     minimum of an unmastered spell, so the printed range is always TRUE, and a
//     mastered spell simply lands in its upper part. (The physical row has it easier -
//     it reads the equipped weapon's mastery via `call 0x005D59D6` @0x008C2A2B.)
//
// Like the physical row, the range is quoted at 100% skill damage: the client's own
// `fimul [ebp+0x30]` (@0x00791C63, and in the cave) is the per-skill multiplier and is
// not part of a character-level figure.
// ============================================================

namespace {

constexpr uintptr_t kAddr_CWvsContextSlot = 0x00BE7918;
constexpr uintptr_t kAddr_CUserLocalSlot  = 0x00BEBF98;
constexpr uintptr_t kOff_WeaponItemID     = 0x4EC;

// Set false to take the magician RANGE row back out in one line, leaving the
// physical estimate (still 64-bit) in place. The formula cave is unaffected.
constexpr bool kInstallRangeRow = true;

// Stat cache, 12-byte ZtlSecure<long> stride (value, key, checksum at value+8).
constexpr uintptr_t kOff_StatCache = 0x20BC;
constexpr uintptr_t kOff_Job       = 0x0018;
constexpr uintptr_t kOff_Str       = 0x0024;
constexpr uintptr_t kOff_Dex       = 0x0030;
constexpr uintptr_t kOff_Int       = 0x003C;
constexpr uintptr_t kOff_Luk       = 0x0048;

// SecondaryStat, 0x30 stride per stat: MAD is the third block (PAD +0x00, PDD +0x30,
// MAD +0x60), base/cs/bonus/cs at +0/+8/+0xC/+0x14 inside it. These are the very slots
// MDamage fuses at 0x00791644 and 0x00791655.
constexpr uintptr_t kOff_SecondaryStat = 0x2134;
constexpr uintptr_t kOff_PadBase       = 0x0000;
constexpr uintptr_t kOff_PadBonus      = 0x000C;
constexpr uintptr_t kOff_MadBase       = 0x0060;
constexpr uintptr_t kOff_MadBonus      = 0x006C;
constexpr uintptr_t kOff_AttackRate    = 0x0768;

// value ^ _rotl(key, 5), deliberately NOT a call to ZtlSecureFuse_long (0x00416563).
//
// A DISPLAY path must not be able to raise. ZtlSecureFuse_long validates the checksum
// word and throws a ZException when it disagrees, and this runs inside
// CUIStatDetail::Draw, where an escaping C++ exception is a silent client death.
// Reading the pair directly cannot throw at all, so the __except below is a backstop
// for a bad pointer rather than the primary defence.
inline int SecureFuseAt(const void* pBase, uintptr_t uOff) {
    const unsigned int* p = reinterpret_cast<const unsigned int*>(
        static_cast<const unsigned char*>(pBase) + uOff);
    return static_cast<int>(p[0] ^ _rotl(p[1], 5));
}

// (job % 1000) / 100 == 2 selects the magician family in Server/.../client/Job.java:
// 200, 210-212, 220-222, 230-232, Blaze Wizard 1200/1210-1212 and Evan 2200/2210-2218,
// with no false positives. critratedisplay.cpp uses the same test.
constexpr int kMagicianJobClass = 2;
constexpr int kThiefJobClass    = 4;

bool g_bMagicFormulaInstalled = false;

// Cosmic WeaponType multipliers, indexed by (itemId/10000)%100 - 30.
// Same table ItemInformationProvider.getWeaponType uses.
double WeaponMultiplier(int nItemId, int nJobClass) {
    const int nCat = (nItemId / 10000) % 100;
    if (nCat < 30 || nCat > 49) {
        return 4.0;
    }
    static const double kMult[] = {
        4.0, 4.4, 4.4, 4.0, 0.0, 0.0, 0.0, 3.6, 3.6, 0.0,
        4.6, 4.8, 4.8, 5.0, 5.0, 3.4, 3.6, 3.6, 4.8, 3.6
    };
    double d = kMult[nCat - 30];
    if (d <= 0.0) {
        return 4.0;
    }
    // Thief daggers use 3.6 (DAGGER_THIEVES); everyone else keeps 4.0.
    if (nCat == 33 && nJobClass == kThiefJobClass) {
        return 3.6;
    }
    return d;
}

void PhysicalMainSecondary(int nItemId, int nJobClass, int nStr, int nDex, int nLuk,
                           int* pnMain, int* pnSecondary) {
    const int nCat = (nItemId / 10000) % 100;
    if (nCat == 45 || nCat == 46 || nCat == 49) {
        *pnMain = nDex;
        *pnSecondary = nStr;
    } else if (nCat == 47 || (nCat == 33 && nJobClass == kThiefJobClass)) {
        *pnMain = nLuk;
        *pnSecondary = nDex + nStr;
    } else {
        *pnMain = nStr;
        *pnSecondary = nDex;
    }
}

} // namespace

// false means the context could not be read. Magicians get the combat formula;
// every other job gets the physical 100%-skill estimate. Both stay in 64-bit so
// the RANGE row cannot wrap to INT_MIN (shown as -2.14b) or the client's
// `if (value <= 1) value = 1` clamp after an overflowing __ftol.
bool MagicDmg_GetRange(long long* pnMin, long long* pnMax) {
    if (pnMin == nullptr || pnMax == nullptr) {
        return false;
    }
    char* pCtx = *reinterpret_cast<char**>(kAddr_CWvsContextSlot);
    if (pCtx == nullptr) {
        return false;
    }

    int nJob = 0;
    int nStr = 0;
    int nDex = 0;
    int nInt = 0;
    int nLuk = 0;
    int nMad = 0;
    int nPad = 0;
    int nRate = 0;
    __try {
        char* pStats = pCtx + kOff_StatCache;
        char* pSec   = pCtx + kOff_SecondaryStat;
        nJob = SecureFuseAt(pStats, kOff_Job);
        nStr = SecureFuseAt(pStats, kOff_Str);
        nDex = SecureFuseAt(pStats, kOff_Dex);
        nInt = SecureFuseAt(pStats, kOff_Int);
        nLuk = SecureFuseAt(pStats, kOff_Luk);
        nMad = SecureFuseAt(pSec, kOff_MadBase) + SecureFuseAt(pSec, kOff_MadBonus);
        nPad = SecureFuseAt(pSec, kOff_PadBase) + SecureFuseAt(pSec, kOff_PadBonus);
        nRate = SecureFuseAt(pSec, kOff_AttackRate);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (nStr < 0) nStr = 0;
    if (nDex < 0) nDex = 0;
    if (nInt < 0) nInt = 0;
    if (nLuk < 0) nLuk = 0;
    if (nMad < 0) nMad = 0;
    if (nPad < 0) nPad = 0;

    const int nJobClass = (nJob % 1000) / 100;
    double dMax = 0.0;
    double dMinRatio = kFloorMin;

    if (kInstallRangeRow && g_bMagicFormulaInstalled && nJobClass == kMagicianJobClass) {
        // Combat captures raw base+bonus MAD (before the % attack rate), so RANGE
        // must not apply nRate here.
        dMax = MaxMagicDamage(nInt, nMad);
    } else {
        int nWeaponId = 0;
        char* pUser = *reinterpret_cast<char**>(kAddr_CUserLocalSlot);
        if (pUser != nullptr) {
            __try {
                nWeaponId = *reinterpret_cast<int*>(pUser + kOff_WeaponItemID);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                nWeaponId = 0;
            }
        }
        int nMain = 0;
        int nSecondary = 0;
        PhysicalMainSecondary(nWeaponId, nJobClass, nStr, nDex, nLuk, &nMain, &nSecondary);
        const double dMult = WeaponMultiplier(nWeaponId, nJobClass);
        double dPad = static_cast<double>(nPad);
        if (nRate > 0) {
            dPad += dPad * static_cast<double>(nRate) / 100.0;
        }
        dMax = ((dMult * static_cast<double>(nMain) + static_cast<double>(nSecondary))
                / 100.0) * dPad;
        dMinRatio = 0.50;
    }

    long long nMax = DamageToLong(dMax);
    long long nMin = DamageToLong(dMax * dMinRatio);
    if (nMin > nMax) {
        nMin = nMax;
    }
    *pnMin = nMin;
    *pnMax = nMax;
    return true;
}

// ============================================================
// ATTACH
// ============================================================

void AttachMagicDamageMod() {
    // 1. Hook ztlSecureFuse to capture INT / magic stats as the
    //    damage function decrypts them.
    ATTACH_HOOK(ztlSecureFuse_orig, ztlSecureFuse_hook);

    // 2. Replace the magic damage formula at 0x00791C41 (CalcDamage::MDamage+0x62A).
    //    9 bytes: `fld [ebp-1Ch]` + `fmul [0AFE8D0h]`, both whole instructions, and
    //    nothing branches into 0x00791C42..0x00791C6B. The cave resumes at 0x00791C6C
    //    with one value above the entry stack where stock leaves two; the tail's
    //    `fstp st(2)` / `fstp st(0)` pair still ends balanced because the entry stack
    //    holds exactly one value on this path.
    static const unsigned char kFormulaSite[] = {
        0xDD, 0x45, 0xE4,                       // fld  qword ptr [ebp-1Ch]
        0xDC, 0x0D, 0xD0, 0xE8, 0xAF, 0x00,     // fmul qword ptr [0AFE8D0h]
    };
    if (memcmp(reinterpret_cast<void*>(0x00791C41), kFormulaSite, sizeof(kFormulaSite)) != 0) {
        ErrorMessage("Magic damage: MDamage formula site at 0x00791C41 does not match - skipping.");
        return;
    }
    PatchJmp(0x00791C41, reinterpret_cast<uintptr_t>(&MagicFormula_cave)); // CalcDamage::MDamage — magic formula
    PatchNop(0x00791C41 + 5, 0x00791C41 + 9);

    // Derived from what is actually in memory, so the stat window can never advertise
    // a formula combat is not running. 0xE9 is the JMP this function just wrote.
    g_bMagicFormulaInstalled =
        (*reinterpret_cast<const unsigned char*>(0x00791C41) == 0xE9);
}
