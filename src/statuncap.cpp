#include "pch.h"
#include "hook.h"

// Lifts the client's 1999 (PAD/PDD/MDD/MAD) and 999 (ACC/EVA) stat clamps to 851,711.
//
// Each site is `mov r32, imm32` followed by `cmp`/`cmov`-style clamping; the release this comes
// from writes FF FE 0C over the immediate's low three bytes, i.e. imm = 0x000CFEFF. That value is
// kept on purpose: GetPAD/GetPDD/GetMDD multiply the stat with a 32-bit imul before clamping, and
// 851,711 x a few hundred percent still fits in an int where INT_MAX would not.
//
// Every address was checked against v83.idb (+ v95.pdb names) on 2026-09-26. Deliberately NOT
// patched, so they stay vanilla:
//   - mob -> player (miss checks sub_79286E / sub_7929CA, mob PDamage/MDamage): 0x007928A1,
//     0x007928FD, 0x007929FE, 0x00792AB9, 0x00793107, 0x00793499.
//   - CUIStatDetail ctor 0x008C510A: statuilayout.cpp owns that byte (window width).

namespace {

constexpr unsigned int kUncapped = 0x000CFEFF; // 851,711

struct StatClampSite {
    uintptr_t uAddress;   // the mov r32, imm32
    unsigned int uCap;    // 1999 or 999
    const char* sWhere;
};

const StatClampSite kSites[] = {
    { 0x00780620, 1999, "SecondaryStat::SetFrom (sub_77F4C9) - PAD" },
    { 0x007806D0,  999, "SecondaryStat::SetFrom - ACC" },
    { 0x00780702,  999, "SecondaryStat::SetFrom - EVA" },
    { 0x0077E055, 1999, "SecondaryStat::GetPAD" },
    { 0x0077E12F, 1999, "SecondaryStat::GetPDD (sub_77E067)" },
    { 0x0077E215, 1999, "SecondaryStat::GetMDD (sub_77E141)" },
    { 0x0078FF5F, 1999, "CalcDamage::PDamage - PAD" },
    { 0x0078E061,  999, "CalcDamage::PDamage - ACC (SecondaryStat+0xC0/+0xCC)" },
    { 0x0078E67D,  999, "CalcDamage::PDamage - MobStat+0x74/+0x78" },
    { 0x0079166C, 1999, "CalcDamage::MDamage - MAD" },
    { 0x00791CD5, 1999, "CalcDamage::MDamage - MAD" },
    { 0x007918FC,  999, "CalcDamage::MDamage - MobStat+0x74/+0x78" },
    { 0x007921C2,  999, "CalcDamage::PDamageSummoned (sub_79216D) - ACC (SecondaryStat+0xC0/+0xCC)" },
    { 0x00792637,  999, "CalcDamage::MDamageSummoned (sub_792595) - 999 stat clamp ([ebp+14h])" },
    { 0x007926DD, 1999, "CalcDamage::MDamageSummoned - MAD" },
};

} // namespace

void AttachStatUncapMod() {
    for (const StatClampSite& site : kSites) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(site.uAddress);
        if (p[0] < 0xB8 || p[0] > 0xBF || *reinterpret_cast<const unsigned int*>(p + 1) != site.uCap) {
            ErrorMessage("Stat uncap: %s at 0x%08X does not match - skipped.", site.sWhere, site.uAddress);
            continue;
        }
        Patch4(site.uAddress + 1, kUncapped); // mov r32, 1999/999 -> 851,711 (see kSites for the function)
    }
}
