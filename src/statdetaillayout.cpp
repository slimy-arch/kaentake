#include "pch.h"
#include "hook.h"
#include "critmodel.h"
#include "magicdmg.h"       // MagicDmg_GetRange
#include "ztl/ztl.h"        // IWzCanvas / IWzFont / Ztl_bstr_t / Ztl_variant_t
#include "wvs/packet.h"     // CInPacket (the rates packet)

#include <intrin.h>
#include <cstdio>
#include <cstring>

namespace {

// IWzCanvas::DrawTextA — v83 VA 0x004277AD, the call the nine CUIStatDetail::Draw sites make
// (0x008C3412 .. 0x008C4459; exactly nine `call 0x004277AD` in that function).
constexpr uintptr_t kAddr_DrawTextA = 0x004277AD;
typedef unsigned int(__thiscall* t_DrawTextA)(void* pCanvas, int nLeft, int nTop, void* pText,
                                              void* pFont, void* pVAlpha, void* pVTabOrg);
auto DrawTextA_real = reinterpret_cast<t_DrawTextA>(kAddr_DrawTextA);

// Ztl_bstr_t::Data_t::Release — __thiscall(Data_t*), interlocked decrement of
// the refcount at +8, frees at zero. DrawTextA takes its bstr BY VALUE and
// releases it itself (the client emits no destructor for it at 0x008C341B and
// friends), so on the one path where we do not forward the caller's string we
// have to release it exactly as DrawTextA would.
auto BstrDataRelease = reinterpret_cast<unsigned long(__thiscall*)(void*)>(0x00402EA5);

// ZtlSecureFuse_long __cdecl(ptr, checksum) — how every secured stat is read.
auto ZtlSecureFuse_long = reinterpret_cast<int(__cdecl*)(const void*, int)>(0x00416563);

constexpr uintptr_t kAddr_CWvsContextSlot = 0x00BE7918;
constexpr uintptr_t kOff_SecondaryStat    = 0x2134;   // CWvsContext -> SecondaryStat
constexpr uintptr_t kOff_PAD              = 0x0000;   // weapon attack: base/cs/bonus/cs
constexpr uintptr_t kOff_PAD_Checksum     = 0x0008;
constexpr uintptr_t kOff_PAD_Bonus        = 0x000C;
constexpr uintptr_t kOff_PAD_BonusCheck   = 0x0014;
constexpr uintptr_t kOff_AttackRate       = 0x0768;   // % attack, shared by PAD and MAD
constexpr uintptr_t kOff_AttackRateCheck  = 0x0770;

// --- the grid -------------------------------------------------------------
// Columns sit 5px right of each label plate (plates end at x=75 and x=197 in
// the art); rows are the art's 18px stride, and a value's nTop lines its text
// box up with the plate's centre exactly as the old single-column art did.
constexpr int kColLeft  = 80;
constexpr int kColRight = 202;
// Stat/backgrnd2 is 260 x 185, eight rows:
//   1 RANGE                     5 ACCURACY      | EVASION
//   2 WEAPON ATK.  | MAGIC ATK. 6 CRITICAL RATE | CRITICAL DAM.
//   3 NORMAL DMG   | BOSS DMG   7 DROP RATE     | MESO RATE
//   4 WEAPON DEF.  | MAGIC DEF. 8 SPEED         | JUMP
constexpr int kRow1 = 8, kRow2 = 26, kRow3 = 44, kRow4 = 62, kRow5 = 80, kRow6 = 98,
              kRow7 = 116, kRow8 = 134;

// Server-side values for rows 3 and 7, from SendOpcode.STAT_DETAIL_RATES (0x3740):
// int drop %, int meso %, int normal-monster damage %, int boss damage %.
// Only the server knows them (world rate x coupons x buffs x equipment), so the
// cells read "-" until the first packet arrives.
bool g_bRatesKnown       = false;
int  g_nDropRatePercent  = 0;
int  g_nMesoRatePercent  = 0;
int  g_nNormalDmgPercent = 0;
int  g_nBossDmgPercent   = 0;

// A value this file renders itself rather than taking from the client.
enum Value {
    VALUE_NONE = 0,
    VALUE_WEAPON_ATTACK,     // left cell of row 2  — the client has no draw for it
    VALUE_CRIT_RATE,         // left cell of row 5  — client draws craft here; we override
    VALUE_CRIT_DAMAGE,       // right cell of row 5 — the client has no draw for it
    VALUE_ACCURACY,          // left cell of row 4  — client draws raw ACC; we override
    VALUE_EVASION,           // right cell of row 4 — client draws raw EVA; we override
    VALUE_RANGE,             // row 1 — magic formula for mages, physical estimate
                             // for everyone else (both 64-bit; see FormatValue)
    VALUE_NORMAL_DAMAGE,     // left cell of row 3  — server value (placeholder 0% for now)
    VALUE_BOSS_DAMAGE,       // right cell of row 3 — server value (placeholder 0% for now)
    VALUE_DROP_RATE,         // left cell of row 7  — server value
    VALUE_MESO_RATE,         // right cell of row 7 — server value
};

constexpr int kMaxExtras = 5;

struct StatCell {
    uintptr_t uCallSite;     // the `call DrawTextA` we repoint
    int       nLeft;
    int       nTop;
    Value     eSelf;         // replaces the client's own string for THIS cell
    Value     aeExtra[kMaxExtras]; // additional cells to draw while we are here
    bool      bNarrow;       // one grid cell wide -> collapse the buffed breakdown
    const char* sWhat;       // for the guard's error text
};

// Call sites verified by scanning every E8 rel32 targeting 0x004277AD inside
// CUIStatDetail::Draw — exactly nine, each 0x10 past its own nLeft push.
// RANGE is the one wide cell: row 1's right half is empty in the art, so its
// "min ~ max" string has the full panel to run into.
const StatCell kCells[] = {
    // The cells the client has no draw for ride on the RANGE draw, which runs every frame.
    { 0x008C3412, kColLeft,  kRow1, VALUE_RANGE,     { VALUE_WEAPON_ATTACK, VALUE_NORMAL_DAMAGE,
                                                       VALUE_BOSS_DAMAGE, VALUE_DROP_RATE,
                                                       VALUE_MESO_RATE },  false, "range"      },
    { 0x008C35DB, kColLeft,  kRow4, VALUE_NONE,      {},                   true,  "weapon def" },
    { 0x008C375C, kColRight, kRow2, VALUE_NONE,      {},                   true,  "magic atk"  },
    { 0x008C39FB, kColRight, kRow4, VALUE_NONE,      {},                   true,  "magic def"  },
    { 0x008C3BAE, kColLeft,  kRow5, VALUE_NONE,      {},                   true,  "accuracy"   },
    { 0x008C3D61, kColRight, kRow5, VALUE_NONE,      {},                   true,  "evasion"    },
    { 0x008C3FA0, kColLeft,  kRow6, VALUE_CRIT_RATE, { VALUE_CRIT_DAMAGE }, true,  "crit rate"  },
    { 0x008C436F, kColLeft,  kRow8, VALUE_NONE,      {},                   true,  "speed"      },
    { 0x008C4459, kColRight, kRow8, VALUE_NONE,      {},                   true,  "jump"       },
};

const StatCell* FindCell(uintptr_t uReturnAddress) {
    for (const StatCell& c : kCells) {
        if (uReturnAddress == c.uCallSite + 5) {   // return lands after the call
            return &c;
        }
    }
    return nullptr;
}

// Weapon attack exactly as the client's own GetPAD computes it (the tail of
// that function is 0x0077E01C-0x0077E060): base + buff bonus, then the
// percentage attack rate at +0x768 (+0x770), then clamped to 1999. The MAGIC
// ATK. cell next door applies the same rate field at 0x008C3610, so skipping
// either step would make the two neighbours disagree under a %-attack effect.
int QueryWeaponAttack() {
    char* pCtx = *reinterpret_cast<char**>(kAddr_CWvsContextSlot);
    if (pCtx == nullptr) {
        return 0;
    }
    int nTotal = 0;
    __try {
        char* pStat = pCtx + kOff_SecondaryStat;
        nTotal = ZtlSecureFuse_long(pStat + kOff_PAD,
                                    *reinterpret_cast<int*>(pStat + kOff_PAD_Checksum));
        nTotal += ZtlSecureFuse_long(pStat + kOff_PAD_Bonus,
                                     *reinterpret_cast<int*>(pStat + kOff_PAD_BonusCheck));
        const int nRate = ZtlSecureFuse_long(
            pStat + kOff_AttackRate, *reinterpret_cast<int*>(pStat + kOff_AttackRateCheck));
        if (nRate > 0) {
            nTotal += nTotal * nRate / 100;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (nTotal < 0) {
        nTotal = 0;
    }
    return nTotal;
}

// The buffed rows do not print a bare number: they format string-pool entry
// 0x793 with THREE integers — total, base and bonus (the push order at e.g.
// 0x008C37F6 is bonus, base, total under the format). That breakdown needs
// roughly 90px; a grid cell has 55, so in the two-column art it would paint
// over the neighbouring label plate or run off the canvas. For those cells we
// draw only the leading number — which the same push order proves is the
// TOTAL — and let the client's own buffed FONT keep signalling the buff.
bool HasBreakdown(const wchar_t* pStr) {
    for (const wchar_t* p = pStr; *p != 0; ++p) {
        if (*p == L'(') {
            return true;
        }
    }
    return false;
}

bool LeadingInteger(const wchar_t* pStr, wchar_t* sOut, size_t uOutChars) {
    size_t nOut = 0;
    size_t i = 0;
    if (pStr[0] == L'-') {
        sOut[nOut++] = pStr[0];
        i = 1;
    }
    bool bAnyDigit = false;
    for (; pStr[i] >= L'0' && pStr[i] <= L'9' && nOut + 1 < uOutChars; ++i) {
        sOut[nOut++] = pStr[i];
        bAnyDigit = true;
    }
    sOut[nOut] = 0;
    return bAnyDigit;
}

// Same, for the wide-char string the collapse path builds from the client's own.
unsigned int DrawOwnTextW(void* pCanvas, int nLeft, int nTop, const wchar_t* pStr,
                          void* pFont, void* pVAlpha, void* pVTabOrg) {
    unsigned int uWidth = 0;
    try {
        IWzCanvas* pWzCanvas = reinterpret_cast<IWzCanvas*>(pCanvas);
        IWzFont* pWzFont = reinterpret_cast<IWzFont*>(pFont);
        if (pVAlpha != nullptr && pVTabOrg != nullptr) {
            uWidth = pWzCanvas->DrawTextA(nLeft, nTop, Ztl_bstr_t(pStr), pWzFont,
                                          *reinterpret_cast<Ztl_variant_t*>(pVAlpha),
                                          *reinterpret_cast<Ztl_variant_t*>(pVTabOrg));
        } else {
            uWidth = pWzCanvas->DrawTextA(nLeft, nTop, Ztl_bstr_t(pStr), pWzFont,
                                          Ztl_variant_t(), Ztl_variant_t());
        }
    } catch (...) {
        // A missed cell is a cosmetic loss; never take the window down for it.
    }
    return uWidth;
}

// RANGE row: at most 4 digits across denomination parts.
// 1,234,123 -> "1M 234k". A lower unit is omitted when its full count
// would exceed the budget, except k/M/B/T/Q/Qu which may be shortened
// to use leftover slots ("12M 34k" for 12,345,678). Ones are never truncated.
int DigitCount(long long n) {
    if (n < 0) {
        n = -n;
    }
    if (n == 0) {
        return 1;
    }
    int nDigits = 0;
    while (n > 0) {
        ++nDigits;
        n /= 10;
    }
    return nDigits;
}

long long LeadingDigits(long long n, int nKeep) {
    if (nKeep <= 0) {
        return 0;
    }
    int nDigits = DigitCount(n);
    while (nDigits > nKeep) {
        n /= 10;
        --nDigits;
    }
    return n;
}

void AppendDenomPart(char* sOut, size_t uOutChars, size_t* puUsed, long long nCount,
                     const char* sSuffix) {
    char sPart[32];
    if (sSuffix[0] == 0) {
        sprintf_s(sPart, "%lld", nCount);
    } else {
        sprintf_s(sPart, "%lld%s", nCount, sSuffix);
    }
    const size_t uPart = strlen(sPart);
    if (*puUsed > 0) {
        if (*puUsed + 1 + uPart >= uOutChars) {
            return;
        }
        sOut[(*puUsed)++] = ' ';
        sOut[*puUsed] = 0;
    }
    if (*puUsed + uPart >= uOutChars) {
        return;
    }
    memcpy(sOut + *puUsed, sPart, uPart + 1);
    *puUsed += uPart;
}

void FormatDenom(long long n, char* sOut, size_t uOutChars) {
    if (uOutChars == 0) {
        return;
    }
    if (n <= 0) {
        sprintf_s(sOut, uOutChars, "0");
        return;
    }

    static const struct {
        long long nUnit;
        const char* sSuffix;
        bool bAllowTrunc;
    } kUnits[] = {
        { 1000000000000000000LL, "Qu", true },
        { 1000000000000000LL,    "Q",  true },
        { 1000000000000LL,       "T",  true },
        { 1000000000LL,          "B",  true },
        { 1000000LL,             "M",  true },
        { 1000LL,                "k",  true },
        { 1LL,                   "",   false },
    };

    sOut[0] = 0;
    size_t uUsed = 0;
    int nDigitsUsed = 0;
    long long nLeft = n;

    for (const auto& unit : kUnits) {
        if (nDigitsUsed >= 4) {
            break;
        }
        if (nLeft < unit.nUnit) {
            continue;
        }
        long long nCount = nLeft / unit.nUnit;
        const int nCountDigits = DigitCount(nCount);
        const int nBudget = 4 - nDigitsUsed;
        if (nCountDigits <= nBudget) {
            AppendDenomPart(sOut, uOutChars, &uUsed, nCount, unit.sSuffix);
            nDigitsUsed += nCountDigits;
            nLeft %= unit.nUnit;
            continue;
        }
        if (!unit.bAllowTrunc) {
            break;
        }
        const long long nShown = LeadingDigits(nCount, nBudget);
        if (nShown <= 0) {
            break;
        }
        AppendDenomPart(sOut, uOutChars, &uUsed, nShown, unit.sSuffix);
        break;
    }
    if (uUsed == 0) {
        sprintf_s(sOut, uOutChars, "0");
    }
}

// The text for a value we render ourselves. Both crit numbers carry a '%' — they
// are percentages, and the window's SPEED/JUMP cells already read that way.
//
// RETURNS FALSE when this file has nothing to say for the cell, in which case the
// caller must fall back to the client's own string rather than draw an empty or zero
// value. Only VALUE_RANGE currently declines: it is a magician-only override.
bool FormatValue(Value eValue, char* sOut, size_t uOutChars) {
    switch (eValue) {
    case VALUE_RANGE: {
        // Magicians: the combat formula. Everyone else: physical 100%-skill
        // estimate. Both come back as 64-bit so an overflowed int32 cannot
        // print as -2.14b / 2.14b or collapse to 1.
        long long nMin = 0;
        long long nMax = 0;
        if (!MagicDmg_GetRange(&nMin, &nMax)) {
            return false;
        }
        char sMin[32];
        char sMax[32];
        FormatDenom(nMin, sMin, sizeof(sMin));
        FormatDenom(nMax, sMax, sizeof(sMax));
        sprintf_s(sOut, uOutChars, "%s ~ %s", sMin, sMax);
        break;
    }
    case VALUE_WEAPON_ATTACK:
        sprintf_s(sOut, uOutChars, "%d", QueryWeaponAttack());
        break;
    // Magicians fight with spells, and MDamage never rolls a crit: a rate here would
    // promise something their attacks cannot do.
    case VALUE_CRIT_RATE:
        if (CritDisplay_IsMagician()) {
            sprintf_s(sOut, uOutChars, "-");
        } else {
            sprintf_s(sOut, uOutChars, "%d%%", CritDisplay_GetCritRate());
        }
        break;
    case VALUE_CRIT_DAMAGE:
        if (CritDisplay_IsMagician()) {
            sprintf_s(sOut, uOutChars, "-");
        } else {
            sprintf_s(sOut, uOutChars, "%d%%", CritDisplay_GetCritDamage());
        }
        break;
    case VALUE_NORMAL_DAMAGE:
    case VALUE_BOSS_DAMAGE:
    case VALUE_DROP_RATE:
    case VALUE_MESO_RATE: {
        if (!g_bRatesKnown) {
            sprintf_s(sOut, uOutChars, "-");
            break;
        }
        const int nPercent = eValue == VALUE_NORMAL_DAMAGE ? g_nNormalDmgPercent
                           : eValue == VALUE_BOSS_DAMAGE   ? g_nBossDmgPercent
                           : eValue == VALUE_DROP_RATE     ? g_nDropRatePercent
                                                           : g_nMesoRatePercent;
        sprintf_s(sOut, uOutChars, "%d%%", nPercent);
        break;
    }
    default:
        sOut[0] = 0;
        return false;
    }
    return true;
}

// One text draw with our own string, reusing the host row's font and colour
// variants so it is indistinguishable from the client's own numbers.
unsigned int DrawValueText(void* pCanvas, int nLeft, int nTop, const char* sText,
                           void* pFont, void* pVAlpha, void* pVTabOrg) {
    unsigned int uWidth = 0;
    try {
        IWzCanvas* pWzCanvas = reinterpret_cast<IWzCanvas*>(pCanvas);
        IWzFont* pWzFont = reinterpret_cast<IWzFont*>(pFont);
        if (pVAlpha != nullptr && pVTabOrg != nullptr) {
            uWidth = pWzCanvas->DrawTextA(nLeft, nTop, Ztl_bstr_t(sText), pWzFont,
                                          *reinterpret_cast<Ztl_variant_t*>(pVAlpha),
                                          *reinterpret_cast<Ztl_variant_t*>(pVTabOrg));
        } else {
            uWidth = pWzCanvas->DrawTextA(nLeft, nTop, Ztl_bstr_t(sText), pWzFont,
                                          Ztl_variant_t(), Ztl_variant_t());
        }
    } catch (...) {
        // A missed cell is a cosmetic loss; never take the window down for it.
    }
    return uWidth;
}

// The second cell some rows carry, at its own fixed grid position.
void DrawExtraCell(void* pCanvas, void* pFont, void* pVAlpha, void* pVTabOrg, Value eExtra) {
    int nLeft = 0;
    int nTop = 0;
    switch (eExtra) {
    case VALUE_WEAPON_ATTACK: nLeft = kColLeft;  nTop = kRow2; break;
    case VALUE_NORMAL_DAMAGE: nLeft = kColLeft;  nTop = kRow3; break;
    case VALUE_BOSS_DAMAGE:   nLeft = kColRight; nTop = kRow3; break;
    case VALUE_CRIT_DAMAGE:   nLeft = kColRight; nTop = kRow6; break;
    case VALUE_DROP_RATE:     nLeft = kColLeft;  nTop = kRow7; break;
    case VALUE_MESO_RATE:     nLeft = kColRight; nTop = kRow7; break;
    default: return;
    }
    char sText[64];
    if (!FormatValue(eExtra, sText, sizeof(sText))) {
        return;
    }
    DrawValueText(pCanvas, nLeft, nTop, sText, pFont, pVAlpha, pVTabOrg);
}

} // namespace

// SendOpcode.STAT_DETAIL_RATES (0x3740). Routed from clientsocket.cpp, which only Peek2s,
// so the opcode is skipped here.
void StatDetail_HandleRatesPacket(CInPacket* pPacket) {
    pPacket->Decode<unsigned short>();
    g_nDropRatePercent  = pPacket->Decode<int>();
    g_nMesoRatePercent  = pPacket->Decode<int>();
    g_nNormalDmgPercent = pPacket->Decode<int>();
    g_nBossDmgPercent   = pPacket->Decode<int>();
    g_bRatesKnown = true;
}

// __fastcall so ECX carries the canvas (the sites call __thiscall) and the six
// stack args are callee-cleaned, matching DrawTextA's `ret 0x18`.
unsigned int __fastcall StatDetailDrawText_hook(void* pCanvas, void* /*edx*/, int nLeft, int nTop,
                                                void* pText, void* pFont,
                                                void* pVAlpha, void* pVTabOrg) {
    const StatCell* pCell = FindCell(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    if (pCell == nullptr) {
        // Not one of ours — draw exactly where the caller asked.
        return DrawTextA_real(pCanvas, nLeft, nTop, pText, pFont, pVAlpha, pVTabOrg);
    }

    // Cells we source ourselves (the crit numbers) ignore the client's string
    // entirely. DrawTextA takes its bstr BY VALUE and frees it, so on every path
    // that skips the real call we must release it in its place or leak one
    // string per frame.
    unsigned int uWidth = 0;
    bool bDrawn = false;

    // A cell may DECLINE (VALUE_RANGE does, for every non-magician), in which case
    // nothing has been drawn and nothing has been released - the client's own string
    // must still go through the paths below exactly as it did before.
    if (pCell->eSelf != VALUE_NONE) {
        char sText[64];
        if (FormatValue(pCell->eSelf, sText, sizeof(sText))) {
            uWidth = DrawValueText(pCanvas, pCell->nLeft, pCell->nTop, sText,
                                   pFont, pVAlpha, pVTabOrg);
            if (pText != nullptr) {
                BstrDataRelease(pText);
            }
            bDrawn = true;
        }
    }

    // A buffed value in a one-cell-wide slot: draw the total ourselves instead
    // of the breakdown that would not fit.
    if (!bDrawn) {
        const wchar_t* pStr =
            pText != nullptr ? *reinterpret_cast<const wchar_t* const*>(pText) : nullptr;
        wchar_t sTotal[16];
        if (pCell->bNarrow && pStr != nullptr && HasBreakdown(pStr) &&
            LeadingInteger(pStr, sTotal, 16)) {
            uWidth = DrawOwnTextW(pCanvas, pCell->nLeft, pCell->nTop, sTotal,
                                  pFont, pVAlpha, pVTabOrg);
            BstrDataRelease(pText);
            bDrawn = true;
        }
    }

    if (!bDrawn) {
        uWidth = DrawTextA_real(pCanvas, pCell->nLeft, pCell->nTop,
                                pText, pFont, pVAlpha, pVTabOrg);
    }

    for (Value eExtra : pCell->aeExtra) {
        if (eExtra != VALUE_NONE) {
            DrawExtraCell(pCanvas, pFont, pVAlpha, pVTabOrg, eExtra);
        }
    }
    return uWidth;
}

// ============================================================
// ATTACH
// ============================================================
void AttachStatDetailLayoutMod() {
    // ALL OR NOTHING. Every site is verified before any is written, because a
    // partial relayout is worse than none: the art has no label plates left at
    // the stock coordinates, so a value that kept its old spot would sit under
    // the wrong label, and the two below the new window's 149px would vanish
    // entirely. If the guard ever trips, the window keeps its stock geometry
    // and reads wrong in an obvious, diagnosable way rather than a subtle one.
    for (const StatCell& cell : kCells) {
        // Guard: the site must still be `call 0x004277AD`. Repointing anything
        // else would send unrelated arguments through this hook.
        const unsigned char* p = reinterpret_cast<const unsigned char*>(cell.uCallSite);
        const int nRel = *reinterpret_cast<const int*>(cell.uCallSite + 1);
        const uintptr_t uTarget = cell.uCallSite + 5 + static_cast<uintptr_t>(nRel);
        if (p[0] != 0xE8 || uTarget != kAddr_DrawTextA) {
            ErrorMessage("Stat detail layout: %s draw call at 0x%08X is not "
                         "`call DrawTextA` - skipping the whole two-column relayout "
                         "(the detail window will show stock positions against the "
                         "new art).", cell.sWhat, cell.uCallSite);
            return;
        }
    }
    for (const StatCell& cell : kCells) {
        PatchCall(cell.uCallSite, reinterpret_cast<uintptr_t>(&StatDetailDrawText_hook)); // CUIStatDetail::Draw — DrawTextA -> two-column grid
    }
}
