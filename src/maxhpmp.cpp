#include "pch.h"
#include "hook.h"
#include <intrin.h>

// HP / MaxHP / MP / MaxMP widened from short to int, capped at kHpMpCap.
//
// GW_CharacterStat keeps each stat in a ZtlSecure<short> slot: 4 data bytes (at +0x61 HP, +0x69 MaxHP,
// +0x71 MP, +0x79 MaxMP) followed by a 4-byte checksum. For these four slots only:
//   - the Decode2 that feeds them becomes Decode4 (the server writes an int)
//   - ZtlSecureTear<short> becomes HPMP_FakeTear, which stores the raw int and returns checksum 0
//   - ZtlSecureFuse<short> is hooked: checksum 0 -> return the raw int
//   - every movsx / 16-bit test that consumes the fused value is widened to 32 bits
// Every other ZtlSecure<short> keeps its encryption. BasicStat (CWvsContext+0x211C/0x2128) already stores
// MaxHP/MaxMP as ZtlSecure<long> and needs no Tear/Fuse change.
//
// Addresses are for claude-maple/MapleStory/MapleStory.exe (== Maplestory4G.exe). Each patch is checked
// against the original bytes before it is written. Several sites in "Edits/Increase hpmp cap" were wrong
// (Mystic Door x/y, ranged-attack ptBallStart, Job, AP, equip stats, LUK/INT); they are intentionally absent.

namespace {

constexpr unsigned int kHpMpCap = 999999; // keep in sync with Server GameConstants.HP_MP_CAP

// CInPacket::Decode2 / Decode4 — v83 VA 0x0042470C / 0x00406629
constexpr uintptr_t kDecode2 = 0x0042470C;
constexpr uintptr_t kDecode4 = 0x00406629;
// ZtlSecureTear<short> — v83 VA 0x004E80EB, __fastcall(ECX = value, EDX = slot), returns checksum
constexpr uintptr_t kTearShort = 0x004E80EB;

// ZtlSecureFuse<short> — v83 VA 0x004746DD, __cdecl(slot, checksum). Returns only AX; the upper half of EAX is
// left-over checksum bits.
auto ZtlSecureFuse_short = reinterpret_cast<short(__cdecl*)(const void* at, unsigned int cs)>(0x004746DD);

#ifdef _DEBUG
// Records each distinct caller that reads a raw HP/MP slot, so an unaudited read site shows up in client.log.
uintptr_t g_aRawCallers[256];
size_t g_nRawCallers;

void TraceRawCaller(uintptr_t uCaller) {
    for (size_t i = 0; i < g_nRawCallers; ++i) {
        if (g_aRawCallers[i] == uCaller) {
            return;
        }
    }
    if (g_nRawCallers < _countof(g_aRawCallers)) {
        g_aRawCallers[g_nRawCallers++] = uCaller;
        LogMessage("[maxhpmp] raw HP/MP Fuse from 0x%08X", uCaller - 5);
    }
}
#endif

int __cdecl ZtlSecureFuse_short_hook(const void* at, unsigned int cs) {
    if (cs == 0) {
#ifdef _DEBUG
        TraceRawCaller(reinterpret_cast<uintptr_t>(_ReturnAddress()));
#endif
        return *static_cast<const int*>(at);
    }
    // Sign-extend so a widened movsx after an ordinary short stat still reads the right value.
    return ZtlSecureFuse_short(at, cs);
}

unsigned int __fastcall HPMP_FakeTear(int nValue, void* at) {
    *static_cast<int*>(at) = nValue;
    return 0;
}

bool BytesMatch(uintptr_t uAddress, const void* pExpected, size_t uSize) {
    if (memcmp(reinterpret_cast<const void*>(uAddress), pExpected, uSize) == 0) {
        return true;
    }
#ifdef _DEBUG
    ErrorMessage("[maxhpmp] unexpected bytes at 0x%08X, patch skipped", uAddress);
#else
    LogMessage("[maxhpmp] unexpected bytes at 0x%08X, patch skipped", uAddress);
#endif
    return false;
}

// Replace bytes only if the original instruction is still there.
void PatchExpect(uintptr_t uAddress, const char* sExpected, const char* sValue, size_t uSize) {
    if (BytesMatch(uAddress, sExpected, uSize)) {
        PatchMemory(reinterpret_cast<void*>(uAddress), const_cast<char*>(sValue), uSize);
    }
}

#define PATCH_EXPECT(ADDRESS, EXPECTED, VALUE) \
    static_assert(sizeof(EXPECTED) == sizeof(VALUE), "patch size mismatch"); \
    PatchExpect(ADDRESS, EXPECTED, VALUE, sizeof(EXPECTED) - 1)

// Redirect a call only if it still targets uExpected.
void PatchCallExpect(uintptr_t uAddress, uintptr_t uExpected, const void* pDestination) {
    unsigned char abExpected[5] = { 0xE8 };
    unsigned int uRel = static_cast<unsigned int>(uExpected - uAddress - 5);
    memcpy(abExpected + 1, &uRel, sizeof(uRel));
    if (BytesMatch(uAddress, abExpected, sizeof(abExpected))) {
        PatchCall(uAddress, pDestination);
    }
}

void Patch4Expect(uintptr_t uAddress, unsigned int uExpected, unsigned int uValue) {
    if (BytesMatch(uAddress, &uExpected, sizeof(uExpected))) {
        Patch4(uAddress, uValue);
    }
}

} // namespace


void AttachMaxHpMpMod() {
    ATTACH_HOOK(ZtlSecureFuse_short, ZtlSecureFuse_short_hook); // ZtlSecureFuse<short> — raw int for HP/MP slots

    // Decode2 -> Decode4: packets the server now writes as int
    const uintptr_t aDecodeSites[] = {
        0x004E2B9A, 0x004E2BAE, 0x004E2BC2, 0x004E2BD6, // GW_CharacterStat::Decode — HP, MaxHP, MP, MaxMP
        0x004E30DA, 0x004E30F4, 0x004E310E, 0x004E3128, // GW_CharacterStat::DecodeChangeStat — 0x400, 0x800, 0x1000, 0x2000
        0x0077621A,                                     // CStage::OnSetField — HP
    };
    for (uintptr_t uSite : aDecodeSites) {
        PatchCallExpect(uSite, kDecode2, reinterpret_cast<void*>(kDecode4));
    }

    // ZtlSecureTear<short> -> HPMP_FakeTear: every write into an HP/MP slot
    const uintptr_t aTearSites[] = {
        0x004E2BA4, 0x004E2BB8, 0x004E2BCC, 0x004E2BE0, // GW_CharacterStat::Decode — HP, MaxHP, MP, MaxMP
        0x004E30E4, 0x004E30FE, 0x004E3118, 0x004E3132, // GW_CharacterStat::DecodeChangeStat — HP, MaxHP, MP, MaxMP
        0x00776224,                                     // CStage::OnSetField — HP
        0x007646F2, 0x0076470F,                         // CSkillInfo::CheckConsumeForActiveSkill — HP/MP after cost
        0x00967B94, 0x00967BA2,                         // CUserLocal::DoActiveSkill — HP/MP restore on failure
        0x0078D914, 0x0078D961,                         // sub_78D46C (stat validate) — clamped MaxHP/MaxMP
        0x0078D383, 0x0078D39E,                         // client level-up path — HP = MaxHP, MP = MaxMP
    };
    for (uintptr_t uSite : aTearSites) {
        PatchCallExpect(uSite, kTearShort, &HPMP_FakeTear);
    }

    // movsx r32, r16 -> mov r32, r32
    PATCH_EXPECT(0x00554AFC, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // CField_Dojang::Update — HP
    PATCH_EXPECT(0x00554B31, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // CField_Dojang::Update — HP
    PATCH_EXPECT(0x0064286B, "\x0F\xBF\xF0", "\x8B\xF0\x90"); // CUserLocal map damage (sub_642710) — HP
    PATCH_EXPECT(0x00764401, "\x0F\xBF\xC8", "\x8B\xC8\x90"); // CSkillInfo::CheckConsumeForActiveSkill — HP
    PATCH_EXPECT(0x007644E4, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // CSkillInfo::CheckConsumeForActiveSkill — HP
    PATCH_EXPECT(0x00764507, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // CSkillInfo::CheckConsumeForActiveSkill — MP
    PATCH_EXPECT(0x0077ED97, "\x0F\xBF\xC8", "\x8B\xC8\x90"); // BasicStat::SetFrom (sub_77EC9F) — MaxHP
    PATCH_EXPECT(0x0077EDB3, "\x0F\xBF\xC8", "\x8B\xC8\x90"); // BasicStat::SetFrom (sub_77EC9F) — MaxMP
    PATCH_EXPECT(0x0078D8FA, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // sub_78D46C (stat validate) — MaxHP
    PATCH_EXPECT(0x0078D947, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // sub_78D46C (stat validate) — MaxMP
    PATCH_EXPECT(0x007A5B42, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // CSummoned::TryDoingHeal — HP
    PATCH_EXPECT(0x007A5BDF, "\x0F\xBF\xCF", "\x8B\xCF\x90"); // CSummoned::TryDoingHeal — HP (di)
    PATCH_EXPECT(0x008C5DC4, "\x0F\xBF\x4D\xD8", "\x8B\x4D\xD8\x90"); // CUIStat::Draw — HP
    PATCH_EXPECT(0x008C5EB9, "\x0F\xBF\x4D\xD8", "\x8B\x4D\xD8\x90"); // CUIStat::Draw — MP
    PATCH_EXPECT(0x008CBFDB, "\x0F\xBF\xCB", "\x8B\xCB\x90"); // CUIStat AP buttons (sub_8CBDDB) — MaxHP (bx)
    PATCH_EXPECT(0x008CC01E, "\x0F\xBF\xCB", "\x8B\xCB\x90"); // CUIStat AP buttons (sub_8CBDDB) — MaxMP (bx)
    PATCH_EXPECT(0x008CC216, "\x0F\xBF\x4D\xE8", "\x8B\x4D\xE8\x90"); // CUIStat AP buttons (sub_8CBDDB) — MaxHP -> cap check
    PATCH_EXPECT(0x008CC296, "\x0F\xBF\x4D\xE8", "\x8B\x4D\xE8\x90"); // CUIStat AP buttons (sub_8CBDDB) — MaxMP -> cap check
    PATCH_EXPECT(0x008CC464, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // sub_8CBDDB — MaxHP getter sub_8CE65F
    PATCH_EXPECT(0x008CC50E, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // sub_8CBDDB — MaxMP getter sub_8CE66E
    PATCH_EXPECT(0x008CC9E5, "\x0F\xBF\x4D\x08", "\x8B\x4D\x08\x90"); // CUIStat values (sub_8CC8DE) — MaxMP
    PATCH_EXPECT(0x008CCAD3, "\x0F\xBF\x4D\xE0", "\x8B\x4D\xE0\x90"); // CUIStat values (sub_8CC8DE) — MaxHP
    PATCH_EXPECT(0x008CDA12, "\x0F\xBF\x4D\xDC", "\x8B\x4D\xDC\x90"); // CUIStat preview (sub_8CD8A0) — MaxMP
    PATCH_EXPECT(0x008CDB29, "\x0F\xBF\x4D\xE0", "\x8B\x4D\xE0\x90"); // CUIStat preview (sub_8CD8A0) — MaxHP
    PATCH_EXPECT(0x008CDF2E, "\x0F\xBF\x4D\xD0", "\x8B\x4D\xD0\x90"); // CUIStat preview (sub_8CD8A0) — MaxMP
    PATCH_EXPECT(0x008CE007, "\x0F\xBF\x4D\xD4", "\x8B\x4D\xD4\x90"); // CUIStat preview (sub_8CD8A0) — MaxHP
    PATCH_EXPECT(0x008D822D, "\x0F\xBF\x4D\xE0", "\x8B\x4D\xE0\x90"); // CUIStatusBar::Draw — MP
    PATCH_EXPECT(0x008D8237, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // CUIStatusBar::Draw — HP
    PATCH_EXPECT(0x009203F7, "\x0F\xBF\x45\xE0", "\x8B\x45\xE0\x90"); // CUIPartyHP::Draw — local HP
    PATCH_EXPECT(0x0094B096, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // CUserLocal::Update — HP
    PATCH_EXPECT(0x0094B230, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // CUserLocal::Update — HP
    PATCH_EXPECT(0x0094BB78, "\x0F\xBF\xCE", "\x8B\xCE\x90"); // CUserLocal::Update — MP (si)
    PATCH_EXPECT(0x0094EA4C, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // CUserLocal (sub_94E88C) — MP
    PATCH_EXPECT(0x009584B6, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // CUserLocal::SetDamaged — HP
    PATCH_EXPECT(0x0095960C, "\x0F\xBF\xC9", "\x8B\xC9\x90"); // CUserLocal::SetDamaged — MP (cx)
    PATCH_EXPECT(0x0095BA30, "\x0F\xBF\x45\xF0", "\x8B\x45\xF0\x90"); // CUserLocal::TryConsumePetHP — HP
    PATCH_EXPECT(0x0095BC7B, "\x0F\xBF\xC3", "\x8B\xC3\x90"); // CUserLocal::TryConsumePetMP — MP (bx)
    PATCH_EXPECT(0x00967733, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // CUserLocal::DoActiveSkill — HP saved for restore
    PATCH_EXPECT(0x0096774F, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // CUserLocal::DoActiveSkill — MP saved for restore
    PATCH_EXPECT(0x0096AE9D, "\x0F\xBF\xC7", "\x8B\xC7\x90"); // CUserLocal::DoActiveSkill_Prepare — HP% (di)
    PATCH_EXPECT(0x00A02F88, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // CWvsContext::TryRecovery — HP
    PATCH_EXPECT(0x00A03180, "\x0F\xBF\xC0", "\x8B\xC0\x90"); // CWvsContext::TryRecovery — MP
    PATCH_EXPECT(0x00A2938D, "\x0F\xBF\x4D\xEC", "\x8B\x4D\xEC\x90"); // CWvsContext::CheckDarkForce — HP
    PATCH_EXPECT(0x00A29412, "\x0F\xBF\xCF", "\x8B\xCF\x90"); // CWvsContext::CheckDarkForce — HP (di)
    PATCH_EXPECT(0x00A29535, "\x0F\xBF\x4D\xE8", "\x8B\x4D\xE8\x90"); // CWvsContext::CheckDragonFury — MP
    PATCH_EXPECT(0x00A29586, "\x0F\xBF\xCF", "\x8B\xCF\x90"); // CWvsContext::CheckDragonFury — MP (di)
    PATCH_EXPECT(0x004E3371, "\x0F\xBF\x4D\xDC", "\x8B\x4D\xDC\x90"); // GW_CharacterStat string (sub_4E3238) — MaxMP
    PATCH_EXPECT(0x004E3376, "\x0F\xBF\x4D\xD8", "\x8B\x4D\xD8\x90"); // GW_CharacterStat string (sub_4E3238) — MaxHP

    // 16-bit test/cmp -> 32-bit
    PATCH_EXPECT(0x00485C1C, "\x66\x85\xC0", "\x85\xC0\x90"); // CWvsContext::CanSendExclRequest — HP > 0
    PATCH_EXPECT(0x00A09687, "\x66\x85\xC0", "\x85\xC0\x90"); // sub_A09662 (CanSendExclRequest variant) — HP > 0
    PATCH_EXPECT(0x0078D36C, "\x66\x85\xC0", "\x85\xC0\x90"); // client level-up path — HP > 0
    PATCH_EXPECT(0x00A02F5F, "\x66\x85\xC0", "\x85\xC0\x90"); // CWvsContext::TryRecovery — HP != 0
    PATCH_EXPECT(0x00A03155, "\x66\x85\xC0", "\x85\xC0\x90"); // CWvsContext::TryRecovery — HP != 0
    PATCH_EXPECT(0x00950610, "\x66\x3B\xF3", "\x3B\xF3\x90"); // CUserLocal::OnResolveMoveAction — HP == 0
    PATCH_EXPECT(0x009506A5, "\x66\x3B\xC3", "\x3B\xC3\x90"); // CUserLocal::OnResolveMoveAction — HP == 0
    PATCH_EXPECT(0x0078D8E7, "\x66\x3B\xC6", "\x3B\xC6\x90"); // sub_78D46C (stat validate) — MaxHP vs cap
    PATCH_EXPECT(0x0078D934, "\x66\x3B\xC6", "\x3B\xC6\x90"); // sub_78D46C (stat validate) — MaxMP vs cap

    // 30000 -> kHpMpCap
    Patch4Expect(0x0078D8D2, 30000, kHpMpCap); // sub_78D46C — mov esi, 30000 (MaxHP/MaxMP cap)
    Patch4Expect(0x0077F1A0, 30000, kHpMpCap); // BasicStat::SetFrom — mov ebx, 30000 (MaxHP/MaxMP after equips/buffs)
    Patch4Expect(0x008CD657, 30000, kHpMpCap); // sub_8CD5D0 — cmp eax, 30000 (AP -> MaxHP button)
    Patch4Expect(0x008CD6EB, 30000, kHpMpCap); // sub_8CD664 — cmp eax, 30000 (AP -> MaxMP button)
}
