#include "pch.h"
#include "hook.h"
#include "damagelong.h"
#include "magicdmg.h"
#include "damageskin.h"

#include <climits>
#include <cstring>
#include <random>

// Damage uncap: 199,999 -> Long.MAX_VALUE per line.
//
// CalcDamage::PDamage (v83 VA 0x0078DF87) and CalcDamage::MDamage (0x00791617) compute every line
// as a double, clamp it to dbl_AFE8A0 (= 199999.0 in this exe) and store it with __ftol. We skip
// the clamp at the line funnels and at get_damage_adjusted_by_elemAttr, and replace the funnel
// __ftol with a converter that stores a tag (damagelong.h) for values that do not fit.
// dbl_AFE8A0 itself is untouched: Snipe (3221007, 0x0078E543 "199999 - rand") and the
// mob -> player funnels still read it.
//
// Wire contract, shared with Cosmic (AbstractDealDamageHandler.parseDamage / PacketCreator.addAttackBody):
//   each melee/shoot/magic line  C2S  [int min(real, INT_MAX) | 0x80000000 if crit][long real]
//   each line of every attack    S2C  [int min(real, INT_MAX) | 0x80000000 if crit][long real]
//   each summon line             C2S + S2C  [int min(real, INT_MAX)][long real]
//   DAMAGE_MONSTER (DoT/poison)  S2C  [int min(dmg, INT_MAX), or -heal][long dmg][int attacker char id]
// Meso Explosion C2S keeps vanilla 4-byte lines (its encode is not hooked); its S2C lines use the
// 12-byte form like the rest. Client and server must ship together.

namespace {

constexpr int kRing = 0x8000;                    // tags 0x7FFF0000..0x7FFF7FFF
constexpr int kIntMax = INT_MAX;                 // unresolvable overflow (display falls back to the int)
constexpr double kLongMaxAsDouble = 9223372036854775807.0; // rounds to 2^63

long long g_aRing[kRing];
int g_nNext = 0;

bool IsTagMag(unsigned int uMag) {
    return uMag >= static_cast<unsigned int>(kDamageTagBase) && uMag < static_cast<unsigned int>(kDamageTagBase + kRing);
}

bool CallTargetIs(uintptr_t uSite, uintptr_t uTarget) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(uSite);
    return p[0] == 0xE8 && uSite + 5 + *reinterpret_cast<const int*>(p + 1) == uTarget;
}

bool BytesAre(uintptr_t uSite, const void* pExpected, size_t uSize) {
    return memcmp(reinterpret_cast<const void*>(uSite), pExpected, uSize) == 0;
}

} // namespace

bool DamageLong_IsTag(int nDamage) {
    return IsTagMag(static_cast<unsigned int>(nDamage) & 0x7FFFFFFFu);
}

long long DamageLong_Resolve(int nDamage) {
    unsigned int uMag = static_cast<unsigned int>(nDamage) & 0x7FFFFFFFu;
    if (IsTagMag(uMag)) {
        return g_aRing[uMag - kDamageTagBase];
    }
    return uMag;
}

int DamageLong_Store(long long nReal) {
    if (nReal <= 0) {
        return 0;
    }
    if (nReal < kDamageTagBase) {
        return static_cast<int>(nReal);
    }
    int nSlot = g_nNext;
    g_nNext = (g_nNext + 1) % kRing;
    g_aRing[nSlot] = nReal;
    return kDamageTagBase + nSlot;
}


// ---------------------------------------------------------------------------------------------------------
// Line funnels: __ftol replacement at 0x00790599 (PDamage) and 0x00791F9E (MDamage)
// ---------------------------------------------------------------------------------------------------------

extern "C" int __stdcall DamageLong_FtolLine(double d) {
    if (!(d > 0.0)) {
        return 0;
    }
    long long nReal = d >= kLongMaxAsDouble ? LLONG_MAX : static_cast<long long>(d);
    return DamageLong_Store(nReal);
}

// Pops ST(0) like __ftol and returns the int (value or tag) in eax. Both callers reload ecx
// right after (mov ecx, [ebp-48h] / [ebp+40h]) and read only eax.
__declspec(naked) void FtolDamageLine() {
    __asm {
        sub     esp, 8
        fstp    qword ptr [esp]
        call    DamageLong_FtolLine     ; stdcall, pops the double
        ret
    }
}


// ---------------------------------------------------------------------------------------------------------
// C2S: the damage-line Encode4 in CUserLocal::TryDoingMelee/Shoot/MagicAttack
// ---------------------------------------------------------------------------------------------------------

// COutPacket::Encode4 — v95 sym, v83 VA 0x004065A6 (thiscall, ret 4)
static auto COutPacket__Encode4 = reinterpret_cast<void(__thiscall*)(void*, unsigned int)>(0x004065A6);

// pSlot points at anDamage[j] of the per-mob attack entry (entry+0x18); abCritical[j] sits 15 ints
// later (entry+0x54), the layout CUserRemote::OnAttack fills at 0x00980628.
extern "C" void __cdecl DamageLong_EncodeLine(void* pPacket, int nDamage, const int* pSlot) {
    long long nReal = DamageLong_Resolve(nDamage);
    unsigned int uWire = nReal >= kIntMax ? static_cast<unsigned int>(kIntMax) : static_cast<unsigned int>(nReal);
    if (pSlot && pSlot[15]) {
        uWire |= 0x80000000u;
    }
    COutPacket__Encode4(pPacket, uWire);
    COutPacket__Encode4(pPacket, static_cast<unsigned int>(nReal));
    COutPacket__Encode4(pPacket, static_cast<unsigned int>(static_cast<unsigned long long>(nReal) >> 32));
}

// TryDoingMeleeAttack 0x00952B28: `mov eax, [ebp+10h]; push [eax]; lea ecx, pkt; call Encode4` - slot in eax.
__declspec(naked) void EncodeDamageLine_Melee() {
    __asm {
        push    eax                     ; pSlot
        push    dword ptr [esp + 8]     ; nDamage (Encode4's argument)
        push    ecx                     ; COutPacket*
        call    DamageLong_EncodeLine
        add     esp, 12
        ret     4
    }
}

// TryDoingShootAttack 0x00955428 / TryDoingMagicAttack 0x0095705E: `push [edi]; lea ecx, pkt; call Encode4`.
__declspec(naked) void EncodeDamageLine_Edi() {
    __asm {
        push    edi                     ; pSlot
        push    dword ptr [esp + 8]     ; nDamage
        push    ecx                     ; COutPacket*
        call    DamageLong_EncodeLine
        add     esp, 12
        ret     4
    }
}


// ---------------------------------------------------------------------------------------------------------
// S2C: the damage-line Decode4 in CUserRemote::OnAttack (0x00980574 Meso Explosion, 0x00980619 others)
// ---------------------------------------------------------------------------------------------------------

// CInPacket::Decode4 — v95 sym, v83 VA 0x00406629 (thiscall)
static auto CInPacket__Decode4 = reinterpret_cast<unsigned int(__thiscall*)(void*)>(0x00406629);

extern "C" unsigned int __stdcall DamageLong_DecodeLine(void* pPacket) {
    unsigned int uLine = CInPacket__Decode4(pPacket);
    unsigned int uLo = CInPacket__Decode4(pPacket);
    unsigned int uHi = CInPacket__Decode4(pPacket);
    long long nReal = static_cast<long long>((static_cast<unsigned long long>(uHi) << 32) | uLo);
    unsigned int uMag = uLine & 0x7FFFFFFFu;
    if (uMag < static_cast<unsigned int>(kDamageTagBase)) {
        return uLine;
    }
    // Values in the tag band (including the INT_MAX sentinel) must become tags, or the Format hook
    // would read a real 2,147,4xx,xxx as a ring index.
    if (nReal < static_cast<long long>(uMag)) {
        nReal = uMag;
    }
    return static_cast<unsigned int>(DamageLong_Store(nReal)) | (uLine & 0x80000000u);
}

// Both loops call with ecx = CInPacket* and read eax (0x0098061E splits the crit bit off).
__declspec(naked) void DecodeDamageLine_Remote() {
    __asm {
        push    ecx
        call    DamageLong_DecodeLine   ; stdcall
        ret
    }
}


// CSummoned::TryDoingAttackManual 0x007A5997: `push [esi+18h]; lea ecx, pkt; call Encode4`. Summons
// have no crit flag, so no slot is passed.
__declspec(naked) void EncodeDamageLine_Summon() {
    __asm {
        push    0                       ; pSlot: no abCritical
        push    dword ptr [esp + 8]     ; nDamage
        push    ecx                     ; COutPacket*
        call    DamageLong_EncodeLine
        add     esp, 12
        ret     4
    }
}

// CMob::OnDamaged (0x0066C6C2) reads DAMAGE_MONSTER, which the server also uses for heals as
// -heal: a negative int is passed through untouched (the sign is not a crit bit here). The trailing
// attacker id picks the damage skin for the ShowDamage call right after (0x0066C6E4).
static int g_nOnDamagedAttacker = 0;

extern "C" unsigned int __stdcall DamageLong_DecodeSignedLine(void* pPacket) {
    unsigned int uLine = CInPacket__Decode4(pPacket);
    unsigned int uLo = CInPacket__Decode4(pPacket);
    unsigned int uHi = CInPacket__Decode4(pPacket);
    g_nOnDamagedAttacker = static_cast<int>(CInPacket__Decode4(pPacket));
    long long nReal = static_cast<long long>((static_cast<unsigned long long>(uHi) << 32) | uLo);
    if (static_cast<int>(uLine) < kDamageTagBase) {
        return uLine;
    }
    if (nReal < static_cast<long long>(uLine)) {
        nReal = uLine;
    }
    return static_cast<unsigned int>(DamageLong_Store(nReal));
}

__declspec(naked) void DecodeDamageLine_Signed() {
    __asm {
        push    ecx
        call    DamageLong_DecodeSignedLine
        ret
    }
}

// CMob::ShowDamage — v95 sym, v83 VA 0x006691D3 (thiscall, ret 10h: 4 args in v83, 6 in the v95 name).
static auto CMob__ShowDamage = reinterpret_cast<void(__thiscall*)(void*, int, int, int, int)>(0x006691D3);

// CMob::OnDamaged's draw (0x0066C6E4), scoped to the attacker's damage skin.
static void __fastcall CMob__ShowDamage_OnDamaged(void* pMob, void* _EDX, int nDamage, int a2, int a3, int a4) {
    int nSaved = DamageSkin_BeginAttacker(g_nOnDamagedAttacker);
    g_nOnDamagedAttacker = 0;
    CMob__ShowDamage(pMob, nDamage, a2, a3, a4);
    DamageSkin_End(nSaved);
}

// CMob::Update draws Poison / Venom / Ninja Ambush ticks itself every 1000 ms from the mob's status
// value, which travels as a short (so 32,767 at most). The server now broadcasts every tick as
// DAMAGE_MONSTER with the real damage and the attacker, so these three draws would double them.
static void __fastcall CMob__ShowDamage_Suppressed(void* pMob, void* _EDX, int nDamage, int a2, int a3, int a4) {
}


// ---------------------------------------------------------------------------------------------------------
// SKILLENTRY::AdjustDamageDecRate — v95 sym, v83 VA 0x0075BF50 (thiscall, ret 10h)
// ---------------------------------------------------------------------------------------------------------

// Per-target damage decay (Chain Lightning 2221006, Arrow Bomb, Iron Arrow, Piercing Arrow, ...):
// every line of target n is multiplied by a table factor with `fild [esi]; fmul st(1); call __ftol`.
// On a tag that scales the ~2.147e9 index instead of the real damage, so the bounces showed ~INT_MAX.
extern "C" int __stdcall DamageLong_ScaleLine(int nLine, double dFactor) {
    return DamageLong_FtolLine(static_cast<double>(DamageLong_Resolve(nLine)) * dFactor);
}

// Replaces those three instructions (9 bytes at 0x0075BFFB). ST(0) = the factor and must stay on the
// stack for the next line; esi = the line; eax = the result, stored by the following `mov [esi], eax`.
__declspec(naked) void AdjustDamageDecRate_line() {
    __asm {
        sub     esp, 8
        fst     qword ptr [esp]         ; dFactor, not popped
        push    dword ptr [esi]         ; nLine
        call    DamageLong_ScaleLine    ; stdcall, pops 12
        ret
    }
}


// ---------------------------------------------------------------------------------------------------------
// Snipe (3221007) inside CalcDamage::PDamage
// ---------------------------------------------------------------------------------------------------------

// Stock (0x0078E4E7..0x0078E552): 199999 - rand(0..10000) -> anDamage[i], i.e. a fixed ~195k.
// Now: the character's physical max-damage hit (the stat window's 100%-skill estimate, weapon
// multiplier x stats x PAD, see MagicDmg_GetRange) x 3000%, spread over 95..100%, always a crit.
// The server takes the line as sent (AbstractDealDamageHandler no longer rewrites Snipe).
constexpr double kSnipeMultiplier = 30.0;
constexpr double kSnipeSpreadMin = 0.95;

extern "C" int __stdcall DamageLong_SnipeLine(int nIndex, int* abCritical) {
    long long nMin = 0;
    long long nMax = 0;
    if (!MagicDmg_GetRange(&nMin, &nMax) || nMax <= 0) {
        nMax = 199999; // context unreadable: the stock fixed damage
    }
    static std::mt19937 s_rng(std::random_device{}());
    std::uniform_real_distribution<double> spread(kSnipeSpreadMin, 1.0);
    if (abCritical && nIndex >= 0 && nIndex < 15) {
        abCritical[nIndex] = 1;
    }
    return DamageLong_FtolLine(static_cast<double>(nMax) * kSnipeMultiplier * spread(s_rng));
}

// Entered by jmp from 0x0078E4E7 with ebx = &anDamage[i] (0x0078E3D8), i = [ebp+24h],
// abCritical = [ebp+44h]. FPU-neutral like the stock block; resumes at the shared store
// `mov [ebx], eax` (0x007905F8).
static DWORD kSnipeStore = 0x007905F8;

__declspec(naked) void Snipe_cave() {
    __asm {
        inc     dword ptr [ebp - 4]         ; stock: advance the CalcDamage random index
        push    dword ptr [ebp + 0x44]      ; abCritical
        push    dword ptr [ebp + 0x24]      ; i
        call    DamageLong_SnipeLine        ; stdcall -> eax
        jmp     [kSnipeStore]
    }
}


// ---------------------------------------------------------------------------------------------------------
// Summon damage base: PDamageSummoned (sub_79216D) / MDamageSummoned (sub_792595)
// ---------------------------------------------------------------------------------------------------------

// Stock summons never see the player's attack: physical is (0.7..1.0) x main x 2.5 + secondary, magic
// is the vanilla MAD curve, and both are then x the summon skill's own pad/mad (e.g. Phoenix 550,
// Elquines 300) / 100. With PAD/MAD uncapped and magicdmg.cpp's formula, active skills outgrew them
// ~100x. The base is now the same max-hit estimate the stat window and Snipe use (MagicDmg_GetRange:
// physical weapon multiplier x stats x PAD, or the custom magic formula for magicians), rolled
// uniformly over its [min, max]. The stock `x pad|mad / 100` that follows is kept, so a summon's
// pad/mad acts as its skill damage %.
extern "C" double __stdcall DamageLong_SummonBase(double dStockBase) {
    long long nMin = 0;
    long long nMax = 0;
    if (!MagicDmg_GetRange(&nMin, &nMax) || nMax <= 0) {
        return dStockBase;
    }
    static std::mt19937 s_rng(std::random_device{}());
    std::uniform_real_distribution<double> roll(static_cast<double>(nMin), static_cast<double>(nMax));
    return nMin < nMax ? roll(s_rng) : static_cast<double>(nMax);
}

// Both caves replace ST(0) (the stock base) in place, so the FPU depth is unchanged: PDamageSummoned
// keeps its random factor in ST(1), MDamageSummoned two scratch values that 0x00792814/16 pop.
static DWORD kSummonP_Ret = 0x00792518;
static DWORD kSummonM_Ret = 0x00792811;
static double kOneHundredth = 0.01; // same value as the stock operand at 0x00AF14F0

__declspec(naked) void SummonP_base_cave() {
    __asm {
        sub     esp, 8
        fstp    qword ptr [esp]             ; dStockBase (popped)
        call    DamageLong_SummonBase       ; stdcall, result in ST(0)
        fimul   dword ptr [ebp + 0x24]      ; stock: x the summon's pad
        mov     eax, dword ptr [ebp + 0xC]  ; stock
        jmp     [kSummonP_Ret]
    }
}

__declspec(naked) void SummonM_base_cave() {
    __asm {
        sub     esp, 8
        fstp    qword ptr [esp]
        call    DamageLong_SummonBase
        fimul   dword ptr [ebp + 0x24]      ; stock: x the summon's mad
        fmul    kOneHundredth               ; stock: / 100
        jmp     [kSummonM_Ret]
    }
}


// ---------------------------------------------------------------------------------------------------------
// Vanilla Shadow Partner inside PDamage (thief 4111002 / 14111000, e.g. shooting with a claw)
// ---------------------------------------------------------------------------------------------------------

// Stock: partner = anDamage[first-half] * pct / 100 in 32-bit imul/idiv, which wraps on a tag.
// The result feeds [ebp-20h] and then the line funnel at 0x00790586, so returning the real double
// lets the funnel store a tag for it.
extern "C" double __stdcall DamageLong_PartnerPercent(int nSource, int nPercent) {
    long long nReal = DamageLong_Resolve(nSource);
    if (nPercent <= 0 || nReal <= 0) {
        return 0.0;
    }
    long long nPart = nReal <= LLONG_MAX / nPercent ? nReal * nPercent / 100 : nReal / 100 * nPercent;
    return static_cast<double>(nPart);
}

// Entered by jmp over `imul eax, edx ... fild [ebp-8]` (15 bytes). eax = first-half line,
// edx = skill %, ecx = byte offset of that line (read again at 0x00790289 for the crit copy),
// and the stock `pop edi` left edi = 100.
static DWORD kPartnerPct_Ret1 = 0x0079024B;
static DWORD kPartnerPct_Ret2 = 0x00790283;

__declspec(naked) void PartnerPercent_cave1() {
    __asm {
        push    ecx
        push    edx
        push    eax
        call    DamageLong_PartnerPercent   ; stdcall, result in ST(0)
        pop     ecx
        mov     edi, 100
        jmp     [kPartnerPct_Ret1]
    }
}

__declspec(naked) void PartnerPercent_cave2() {
    __asm {
        push    ecx
        push    edx
        push    eax
        call    DamageLong_PartnerPercent
        pop     ecx
        mov     edi, 100
        jmp     [kPartnerPct_Ret2]
    }
}


// ---------------------------------------------------------------------------------------------------------

void AttachDamageLongMod() {
    static const unsigned char kJb08[] = { 0x72, 0x08 };
    static const unsigned char kJb28[] = { 0x72, 0x28 };
    static const unsigned char kPartnerBlock[] = {
        0x0F, 0xAF, 0xC2,       // imul eax, edx
        0x6A, 0x64,             // push 64h
        0x99,                   // cdq
        0x5F,                   // pop edi
        0xF7, 0xFF,             // idiv edi
        0x89, 0x45, 0xF8,       // mov [ebp-8], eax
        0xDB, 0x45, 0xF8,       // fild dword ptr [ebp-8]
    };
    static const unsigned char kDecRateLine[] = { 0xDB, 0x06, 0xD8, 0xC9 };    // fild [esi]; fmul st, st(1)
    static const unsigned char kSnipeBlock[] = { 0x8B, 0x45, 0xFC, 0xD9, 0xEE }; // mov eax, [ebp-4]; fldz
    static const unsigned char kSnipeTail[] = { 0xE9, 0xA1, 0x20, 0x00, 0x00 };  // jmp 0x007905F8
    constexpr uintptr_t kFtol = 0x00A62018;     // __ftol
    constexpr uintptr_t kShowDamage = 0x006691D3; // CMob::ShowDamage
    static const unsigned char kSummonPSite[] = { 0xDA, 0x4D, 0x24, 0x8B, 0x45, 0x0C };              // fimul [ebp+24h]; mov eax, [ebp+0Ch]
    static const unsigned char kSummonMSite[] = { 0xDA, 0x4D, 0x24, 0xDC, 0x0D, 0xF0, 0x14, 0xAF, 0x00 }; // fimul [ebp+24h]; fmul [0AF14F0h]
    constexpr uintptr_t kEncode4 = 0x004065A6;
    constexpr uintptr_t kDecode4 = 0x00406629;

    bool bOk = BytesAre(0x0079058F, kJb08, 2) && BytesAre(0x00791F94, kJb08, 2) && BytesAre(0x007907C0, kJb28, 2)
            && CallTargetIs(0x00790599, kFtol) && CallTargetIs(0x00791F9E, kFtol)
            && CallTargetIs(0x00952B28, kEncode4) && CallTargetIs(0x00955428, kEncode4) && CallTargetIs(0x0095705E, kEncode4)
            && CallTargetIs(0x00980574, kDecode4) && CallTargetIs(0x00980619, kDecode4)
            && BytesAre(0x0079023C, kPartnerBlock, sizeof(kPartnerBlock)) && BytesAre(0x00790274, kPartnerBlock, sizeof(kPartnerBlock))
            && CallTargetIs(0x0079255C, kFtol) && CallTargetIs(0x00792862, kFtol)
            && CallTargetIs(0x007A5997, kEncode4) && CallTargetIs(0x007A6982, kDecode4) && CallTargetIs(0x0066C6D4, kDecode4)
            && BytesAre(0x0075BFFB, kDecRateLine, sizeof(kDecRateLine)) && CallTargetIs(0x0075BFFF, kFtol)
            && BytesAre(0x0078E4E7, kSnipeBlock, sizeof(kSnipeBlock)) && BytesAre(0x0078E552, kSnipeTail, sizeof(kSnipeTail))
            && BytesAre(0x00792512, kSummonPSite, sizeof(kSummonPSite)) && BytesAre(0x00792808, kSummonMSite, sizeof(kSummonMSite))
            && CallTargetIs(0x0066C6E4, kShowDamage)
            && CallTargetIs(0x00667684, kShowDamage) && CallTargetIs(0x006676D2, kShowDamage) && CallTargetIs(0x00667720, kShowDamage);
    if (!bOk) {
        // All-or-nothing: a half-installed mod would put 12-byte lines on the wire for some attacks only.
        ErrorMessage("Damage long: a patch site does not match this MapleStory.exe - damage stays capped at 199,999.");
        return;
    }

    // Skip "clamp to dbl_AFE8A0 (199999.0)" - jb 08 -> jmp 08.
    Patch1(0x0079058F, 0xEB); // CalcDamage::PDamage - line funnel clamp
    Patch1(0x00791F94, 0xEB); // CalcDamage::MDamage - line funnel clamp
    Patch1(0x007907C0, 0xEB); // get_damage_adjusted_by_elemAttr (sub_790782) - weak-element clamp, reached from PDamage and MDamage

    PatchCall(0x00790599, &FtolDamageLine); // CalcDamage::PDamage - line __ftol -> value or tag
    PatchCall(0x00791F9E, &FtolDamageLine); // CalcDamage::MDamage - line __ftol -> value or tag

    PatchCall(0x00952B28, &EncodeDamageLine_Melee); // CUserLocal::TryDoingMeleeAttack - damage line Encode4 -> int + long
    PatchCall(0x00955428, &EncodeDamageLine_Edi);   // CUserLocal::TryDoingShootAttack - damage line Encode4 -> int + long
    PatchCall(0x0095705E, &EncodeDamageLine_Edi);   // CUserLocal::TryDoingMagicAttack - damage line Encode4 -> int + long

    PatchCall(0x00980574, &DecodeDamageLine_Remote); // CUserRemote::OnAttack - Meso Explosion line Decode4 -> int + long
    PatchCall(0x00980619, &DecodeDamageLine_Remote); // CUserRemote::OnAttack - damage line Decode4 -> int + long

    PatchJmp(0x0079023C, &PartnerPercent_cave1);     // CalcDamage::PDamage - vanilla Shadow Partner % (32-bit imul)
    PatchNop(0x0079023C + 5, 0x0079024B);
    PatchJmp(0x00790274, &PartnerPercent_cave2);     // CalcDamage::PDamage - vanilla Shadow Partner % (other level-data field)
    PatchNop(0x00790274 + 5, 0x00790283);

    PatchCall(0x0075BFFB, &AdjustDamageDecRate_line, 9); // SKILLENTRY::AdjustDamageDecRate - per-target decay on the real value

    PatchJmp(0x0078E4E7, &Snipe_cave);               // CalcDamage::PDamage - Snipe: stat-scaled instead of 199999 - rand
    PatchNop(0x0078E4E7 + 5, 0x0078E557);

    // Summons: PDamageSummoned (sub_79216D) / MDamageSummoned (sub_792595) return __ftol(damage),
    // which wraps above 2^31; CSummoned::TryDoingAttackManual stores it at [entry+18h] (0x007A5698).
    PatchCall(0x0079255C, &FtolDamageLine);          // CalcDamage::PDamageSummoned - result __ftol -> value or tag
    PatchCall(0x00792862, &FtolDamageLine);          // CalcDamage::MDamageSummoned - result __ftol -> value or tag
    PatchCall(0x007A5997, &EncodeDamageLine_Summon); // CSummoned::TryDoingAttackManual - damage Encode4 -> int + long
    PatchCall(0x007A6982, &DecodeDamageLine_Remote); // CSummonedPool::OnAttack (sub_7A6882) - damage Decode4 -> int + long
    PatchJmp(0x00792512, &SummonP_base_cave);        // CalcDamage::PDamageSummoned - base = player max-hit estimate
    PatchNop(0x00792512 + 5, 0x00792518);
    PatchJmp(0x00792808, &SummonM_base_cave);        // CalcDamage::MDamageSummoned - base = custom magic formula
    PatchNop(0x00792808 + 5, 0x00792811);

    PatchCall(0x0066C6D4, &DecodeDamageLine_Signed); // CMob::OnDamaged - DoT/poison damage Decode4 -> int + long + attacker
    PatchCall(0x0066C6E4, &CMob__ShowDamage_OnDamaged); // CMob::OnDamaged - draw with the attacker's damage skin

    // CMob::Update client-side DoT ticks (MobStat at CMob+1A0h; flags from MobStat::DecodeTemporary 0x0078B0B1):
    PatchCall(0x00667684, &CMob__ShowDamage_Suppressed); // CMob::Update - Poison tick (+24Ch = MobStat+ACh, flag 0x200)
    PatchCall(0x006676D2, &CMob__ShowDamage_Suppressed); // CMob::Update - Venom tick (+300h = MobStat+160h, flag 0x1000000)
    PatchCall(0x00667720, &CMob__ShowDamage_Suppressed); // CMob::Update - Ninja Ambush tick (+2F0h = MobStat+150h, flag 0x400000)
}
