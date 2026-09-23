#include "pch.h"
#include "hook.h"
#include "wvs/secure.h"

// Shadow Partner for every class: while the buff is active, melee and magic attacks get
// min(n * 2, 15) lines, and each partner line (the second half) mirrors the matching first-half
// line at 50%. This matches the server's check in AbstractDealDamageHandler::parseDamage.
// Ranged attacks are left alone because vanilla TryDoingShootAttack already handles SP.
//
// The partner lines are written after PDamage/MDamage return instead of forcing PDamage's own
// bShadowPartner branch. That branch reads the level data of skill 4111002/14111000, which a
// non-thief does not have.

static constexpr int MAX_DAMAGE_PER_MOB = 15; // anDamage[15]; the packet also stores the count in a 4-bit nibble

bool IsShadowPartner() {
    // CWvsContext::ms_pInstance -> SecondaryStat (+0x2134) -> nShadowPartner_ (+0x42C, checksum +0x434).
    // TryDoingShootAttack reads the same field at 0x00953B21 (v95 SecondaryStat::nShadowPartner_ @ 0x4A4).
    uintptr_t pContext = *reinterpret_cast<uintptr_t*>(0x00BE7918);
    if (!pContext) {
        return false;
    }
    uintptr_t pSecondaryStat = pContext + 0x2134;
    return ZtlSecureFuse<long>(reinterpret_cast<long*>(pSecondaryStat + 0x42C), *reinterpret_cast<unsigned int*>(pSecondaryStat + 0x434)) != 0;
}

int __stdcall ShadowPartnerLines(int nDamagePerMob) {
    if (!IsShadowPartner()) {
        return nDamagePerMob;
    }
    return (std::min)(nDamagePerMob * 2, MAX_DAMAGE_PER_MOB);
}

void MirrorPartnerLines(int* anDamage, int* abCritical, int nDamagePerMob) {
    if (!IsShadowPartner() || nDamagePerMob < 2 || nDamagePerMob > MAX_DAMAGE_PER_MOB) {
        return;
    }
    int nHalf = nDamagePerMob / 2;
    for (int i = nHalf; i < nDamagePerMob; ++i) {
        int nSource = anDamage[i - nHalf];
        anDamage[i] = nSource <= 0 ? 0 : (std::max)(1, nSource / 2);
        abCritical[i] = abCritical[i - nHalf];
    }
}


// --- MELEE ---

static auto CUserLocal__TryDoingMeleeAttack_ret = 0x00951C08;
void __declspec(naked) CUserLocal__TryDoingMeleeAttack_hook() {
    __asm {
        push    ecx
        push    edx
        push    eax                                     ; nDamagePerMob (already < 15 on this path)
        call    ShadowPartnerLines
        pop     edx
        pop     ecx
        mov     [ ebp - 0x58 ], eax                     ; overwritten instruction
        jmp     [ CUserLocal__TryDoingMeleeAttack_ret ] ; skips the overwritten jmp 0x00951C08
    }
}

// CalcDamage::PDamage — v83 VA 0x0078DF87, 20 args, ret 0x50
static auto CalcDamage__PDamage = reinterpret_cast<void(__thiscall*)(void*, void*, void*, void*, unsigned int, void*, void*, int, int, int, int, int, int, void*, int, int*, int*, int, int, int, int)>(0x0078DF87);

void __fastcall CalcDamage__PDamage_hook(void* pThis, void* _EDX, void* cd, void* bs, void* ss, unsigned int dwMobID, void* ms, void* pTemplate, int nDamagePerMob, int nWeaponItemID, int nBulletItemID, int nAttackType, int nAction, int bShadowPartner, void* pSkill, int nSLV, int* anDamage, int* abCritical, int tKeyDown, int nDarkForce, int nAdvancedChargeDamage, int bInvincible) {
    CalcDamage__PDamage(pThis, cd, bs, ss, dwMobID, ms, pTemplate, nDamagePerMob, nWeaponItemID, nBulletItemID, nAttackType, nAction, bShadowPartner, pSkill, nSLV, anDamage, abCritical, tKeyDown, nDarkForce, nAdvancedChargeDamage, bInvincible);
    MirrorPartnerLines(anDamage, abCritical, nDamagePerMob);
}


// --- MAGIC ---

static auto CUserLocal__TryDoingMagicAttack_ret = 0x00955B94;
void __declspec(naked) CUserLocal__TryDoingMagicAttack_hook() {
    __asm {
        cmp     eax, 15                                 ; overwritten cap (jl + mov [ebp-0x64], ecx)
        jle     lines_capped
        mov     eax, 15
    lines_capped:
        push    ecx
        push    edx
        push    eax                                     ; nDamagePerMob
        call    ShadowPartnerLines
        pop     edx
        pop     ecx
        mov     [ ebp - 0x64 ], eax                     ; overwritten instruction
        jmp     [ CUserLocal__TryDoingMagicAttack_ret ]
    }
}

// CalcDamage::MDamage — v83 VA 0x00791617, 15 args, ret 0x3C
static auto CalcDamage__MDamage = reinterpret_cast<void(__thiscall*)(void*, void*, void*, void*, int, void*, int, int, int, void*, int, int*, int*, int, int, int)>(0x00791617);

void __fastcall CalcDamage__MDamage_hook(void* pThis, void* _EDX, void* cd, void* bs, void* ss, int nArg4, void* ms, int nDamagePerMob, int nArg7, int nArg8, void* pSkill, int nSLV, int* anDamage, int* abCritical, int nArg13, int nArg14, int nArg15) {
    CalcDamage__MDamage(pThis, cd, bs, ss, nArg4, ms, nDamagePerMob, nArg7, nArg8, pSkill, nSLV, anDamage, abCritical, nArg13, nArg14, nArg15);
    MirrorPartnerLines(anDamage, abCritical, nDamagePerMob);
}


void AttachShadowPartnerMod() {
    PatchJmp(0x00951C00, &CUserLocal__TryDoingMeleeAttack_hook);  // CUserLocal::TryDoingMeleeAttack - double nDamagePerMob before it is stored
    PatchCall(0x00951E42, &CalcDamage__PDamage_hook);             // CUserLocal::TryDoingMeleeAttack - mirror partner lines after PDamage
    PatchJmp(0x00955B8C, &CUserLocal__TryDoingMagicAttack_hook);  // CUserLocal::TryDoingMagicAttack - double nDamagePerMob, keep the 15 cap
    PatchCall(0x0095689B, &CalcDamage__MDamage_hook);             // CUserLocal::TryDoingMagicAttack - mirror partner lines after MDamage
}
