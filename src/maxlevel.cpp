#include "pch.h"
#include "hook.h"
#include "exptable.h"
#include "wvs/packet.h"
#include "ztl/ztl.h"
#include <climits>
#include <unordered_map>


// Level cap kMaxLevel (exptable.h) + 64-bit EXP.
//
// Packet contract with the server (GW_CharacterStat::Decode / DecodeChangeStat):
//   level  Decode1 -> Decode2
//   exp    Decode4 -> Decode8
//
// The native nLevel stays an unsigned char clamped at 255 so byte-sized logic never wraps; the real
// level is shadowed here and substituted at the display sites. The native EXP field receives the
// real EXP shifted right by ExpShift(level), and get_next_level_exp returns the next-level EXP
// with the same shift, so every native 32-bit ratio (EXP bar, stat window %, pet auto-speak)
// stays correct. Only text sites print the unshifted 64-bit values.

static int g_nLevel = 0;
static long long g_nExp = 0;
static std::unordered_map<uintptr_t, int> g_mLevelByTear; // &GW_CharacterStat::nLevel -> real level
static int g_nUserInfoLevel = 0;                          // last CHAR_INFO level; CUIUserInfo is a singleton

constexpr uintptr_t kLevelOffset = 0x33; // GW_CharacterStat::nLevel (ZtlSecure<unsigned char>)

// CInPacket::Decode2 — v83 VA 0x0042470C
static auto CInPacket__Decode2 = reinterpret_cast<unsigned short(__thiscall*)(CInPacket*)>(0x0042470C);
// CInPacket::DecodeBuffer — v83 VA 0x00432257
static auto CInPacket__DecodeBuffer = reinterpret_cast<void(__thiscall*)(CInPacket*, void*, size_t)>(0x00432257);
// _ZtlSecureFuse<unsigned char> — v83 VA 0x0047465D
static auto _ZtlSecureFuse_uchar = reinterpret_cast<unsigned char(__cdecl*)(const unsigned char*, unsigned int)>(0x0047465D);
// GW_CharacterStat::_ZtlSecureGet_nLevel — v95 sym, v83 VA 0x008D8289
static auto GW_CharacterStat___ZtlSecureGet_nLevel = reinterpret_cast<unsigned char(__thiscall*)(void*)>(0x008D8289);
// ZXString<char>::Format — v83 VA 0x00445B4B (__cdecl, varargs)
static auto ZXString_char__Format = reinterpret_cast<ZXString<char>*(__cdecl*)(ZXString<char>*, const char*, ...)>(0x00445B4B);
// get_next_level_exp — v95 sym, v83 VA 0x0078D166
static auto get_next_level_exp = reinterpret_cast<int(__cdecl*)(int)>(0x0078D166);


static int ExpShift(int nLevel) {
    if (nLevel < kExpTableBase) {
        return 0;
    }
    long long nNext = kExpTable[(std::min)(nLevel, kMaxLevel - 1) - kExpTableBase];
    int k = 0;
    while ((nNext >> k) > INT_MAX) {
        ++k;
    }
    return k;
}

static long long NextLevelExp64(int nLevel) {
    if (nLevel < kExpTableBase) {
        return get_next_level_exp(nLevel);
    }
    if (nLevel >= kMaxLevel) {
        return INT_MAX;
    }
    return kExpTable[nLevel - kExpTableBase];
}

// Native level reads are clamped at 255; only then is the shadow needed.
static int RealLevel(const void* pLevel, int nNative) {
    if (nNative < 255) {
        return nNative;
    }
    auto it = g_mLevelByTear.find(reinterpret_cast<uintptr_t>(pLevel));
    return it != g_mLevelByTear.end() ? it->second : nNative;
}

static int RealLocalLevel(int nNative) {
    return nNative >= 255 && g_nLevel > 255 ? g_nLevel : nNative;
}


int __cdecl get_next_level_exp_hook(int nLevel) {
    int nReal = RealLocalLevel(nLevel);
    if (nReal < kExpTableBase) {
        return get_next_level_exp(nReal);
    }
    return static_cast<int>((std::min)(NextLevelExp64(nReal) >> ExpShift(nReal), static_cast<long long>(INT_MAX)));
}


static unsigned char __cdecl Level_Decode_Impl(void* pStat, CInPacket* pPacket) {
    int nLevel = CInPacket__Decode2(pPacket);
    g_nLevel = nLevel;
    g_mLevelByTear[reinterpret_cast<uintptr_t>(pStat) + kLevelOffset] = nLevel;
    return static_cast<unsigned char>((std::min)(nLevel, 255));
}

// Replaces "call CInPacket::Decode1" for nLevel; ECX = CInPacket*, ESI = GW_CharacterStat* at both sites.
static void __declspec(naked) Level_Decode1To2() {
    __asm {
        push    ecx
        push    esi
        call    Level_Decode_Impl
        add     esp, 8
        ret
    }
}

// Replaces "call CInPacket::Decode4" for nEXP.
int __fastcall Exp_Decode4To8(CInPacket* pPacket, void* _EDX) {
    long long nExp = 0;
    CInPacket__DecodeBuffer(pPacket, &nExp, sizeof(nExp));
    g_nExp = (std::max)(nExp, 0LL);
    return static_cast<int>((std::min)(g_nExp >> ExpShift(g_nLevel), static_cast<long long>(INT_MAX)));
}


// Replaces "call CInPacket::Decode1" for the level in CWvsContext::OnCharacterInfo; the caller stores AL
// into CUIUserInfo::nLevel (byte at +0x66C) via CUIUserInfo::SetUserInfo.
int __fastcall CharInfo_DecodeLevel(CInPacket* pPacket, void* _EDX) {
    g_nUserInfoLevel = CInPacket__Decode2(pPacket);
    return (std::min)(g_nUserInfoLevel, 255);
}

int __cdecl Fuse_Level_hook(const unsigned char* pLevel, unsigned int uCS) {
    return RealLevel(pLevel, _ZtlSecureFuse_uchar(pLevel, uCS));
}

int __fastcall GW_CharacterStat___ZtlSecureGet_nLevel_hook(void* pThis, void* _EDX) {
    return RealLevel(reinterpret_cast<unsigned char*>(pThis) + kLevelOffset, GW_CharacterStat___ZtlSecureGet_nLevel(pThis));
}


// Rewrites the first nWiden "%d" conversions to "%lld". Fails if any other conversion comes first,
// so an unexpected StringPool format falls back to the native call instead of misreading varargs.
static bool WidenFormat(const char* sFormat, int nWiden, char* sOut, size_t uSize) {
    if (!sFormat) {
        return false;
    }
    std::string s;
    int nWidened = 0;
    for (const char* p = sFormat; *p; ++p) {
        if (*p != '%' || nWidened >= nWiden) {
            s += *p;
        } else if (p[1] == '%') {
            s += "%%";
            ++p;
        } else if (p[1] == 'd') {
            s += "%lld";
            ++nWidened;
            ++p;
        } else {
            return false;
        }
    }
    if (nWidened != nWiden || s.size() >= uSize) {
        return false;
    }
    strcpy_s(sOut, uSize, s.c_str());
    return true;
}

// CUIStat::Draw — EXP line, format(exp, percent)
ZXString<char>* __cdecl Format_ExpPercent_hook(ZXString<char>* pThis, const char* sFormat, int nExp, int nPercent) {
    char sWide[256];
    if (!WidenFormat(sFormat, 1, sWide, sizeof(sWide))) {
        return ZXString_char__Format(pThis, sFormat, nExp, nPercent);
    }
    pThis->Format(sWide, g_nExp, nPercent);
    return pThis;
}

// CUIStat::OnMouseMove, CUIStatusBar::ProcessToolTip — format(exp, next)
ZXString<char>* __cdecl Format_ExpNext_hook(ZXString<char>* pThis, const char* sFormat, int nExp, int nNext) {
    char sWide[256];
    if (!WidenFormat(sFormat, 2, sWide, sizeof(sWide))) {
        return ZXString_char__Format(pThis, sFormat, nExp, nNext);
    }
    pThis->Format(sWide, g_nExp, NextLevelExp64(g_nLevel));
    return pThis;
}

// CUIStatusBar::ProcessToolTip — "EXP : %d / %d  GachaponEXP : %d"
ZXString<char>* __cdecl Format_ExpNextGacha_hook(ZXString<char>* pThis, const char* sFormat, int nExp, int nNext, int nGachaExp) {
    char sWide[256];
    if (!WidenFormat(sFormat, 2, sWide, sizeof(sWide))) {
        return ZXString_char__Format(pThis, sFormat, nExp, nNext, nGachaExp);
    }
    pThis->Format(sWide, g_nExp, NextLevelExp64(g_nLevel), nGachaExp);
    return pThis;
}

// CUIStat::Draw — level line, format(level)
ZXString<char>* __cdecl Format_Level_hook(ZXString<char>* pThis, const char* sFormat, int nLevel) {
    return ZXString_char__Format(pThis, sFormat, RealLocalLevel(nLevel));
}

// CUIUserInfo::Draw — level line, format(nLevel byte); not the local player, so use the CHAR_INFO shadow
ZXString<char>* __cdecl Format_UserInfoLevel_hook(ZXString<char>* pThis, const char* sFormat, int nLevel) {
    return ZXString_char__Format(pThis, sFormat, nLevel >= 255 && g_nUserInfoLevel > 255 ? g_nUserInfoLevel : nLevel);
}

// CUIStatusBar::SetNumberValue — EXP number drawn on the gauge
char* __cdecl itoa_Exp_hook(int nValue, char* sBuffer, int nRadix) {
    _i64toa_s(g_nExp, sBuffer, 0x40, nRadix);
    return sBuffer;
}


void AttachMaxLevelMod() {
    // Packet decode
    PatchCall(0x004E2B20, &Level_Decode1To2); // GW_CharacterStat::Decode — nLevel Decode1 -> Decode2
    PatchCall(0x004E303E, &Level_Decode1To2); // GW_CharacterStat::DecodeChangeStat — LEVEL (0x10) Decode1 -> Decode2
    PatchCall(0x004E2C6E, &Exp_Decode4To8);   // GW_CharacterStat::Decode — nEXP Decode4 -> Decode8
    PatchCall(0x004E31B6, &Exp_Decode4To8);   // GW_CharacterStat::DecodeChangeStat — EXP (0x10000) Decode4 -> Decode8

    ATTACH_HOOK(get_next_level_exp, get_next_level_exp_hook);

    // Level display
    PatchCall(0x008D8171, &GW_CharacterStat___ZtlSecureGet_nLevel_hook); // CUIStatusBar::Draw — level for SetStatusValue
    PatchNop(0x008D8176, 0x008D8179);                                     // CUIStatusBar::Draw — drop movzx eax, al
    PatchCall(0x00602DD6, &Fuse_Level_hook);                              // character select — level text
    PatchNop(0x00602DDD, 0x00602DE0);                                     // character select — drop movzx eax, al
    PatchCall(0x006077E9, &Fuse_Level_hook);                              // character select (view all) — level text
    PatchNop(0x006077F3, 0x006077F6);                                     // character select (view all) — drop movzx eax, al
    PatchCall(0x008C5C11, &Format_Level_hook);                            // CUIStat::Draw — level line
    PatchCall(0x00A23760, &CharInfo_DecodeLevel);                         // CWvsContext::OnCharacterInfo — level Decode1 -> Decode2
    PatchCall(0x00901300, &Format_UserInfoLevel_hook);                    // CUIUserInfo::Draw — level line

    // CWvsContext::OnStatChanged — old/new level compare for the LevelUp effect, so 255 -> 256 still counts
    PatchCall(0x00A1FBAC, &Fuse_Level_hook);      // CWvsContext::OnStatChanged — old level
    PatchStr(0x00A1FBB4, "\x8B\xF8\x90");         // CWvsContext::OnStatChanged — movzx edi, al -> mov edi, eax
    PatchCall(0x00A1FC03, &Fuse_Level_hook);      // CWvsContext::OnStatChanged — new level
    PatchNop(0x00A1FC08, 0x00A1FC0B);             // CWvsContext::OnStatChanged — drop movzx eax, al

    // EXP text
    PatchCall(0x008C5FE3, &Format_ExpPercent_hook);   // CUIStat::Draw — EXP line
    PatchCall(0x008C539D, &Format_ExpNext_hook);      // CUIStat::OnMouseMove — EXP tooltip
    PatchCall(0x008D78E3, &Format_ExpNext_hook);      // CUIStatusBar::ProcessToolTip — EXP tooltip
    PatchCall(0x008D789F, &Format_ExpNextGacha_hook); // CUIStatusBar::ProcessToolTip — EXP + Gachapon tooltip
    Patch1(0x008DA407, 0x40);                         // CUIStatusBar::SetNumberValue — alloca 0x20 -> 0x40 for the EXP digits
    PatchCall(0x008DA418, &itoa_Exp_hook);            // CUIStatusBar::SetNumberValue — _itoa(exp) -> _i64toa_s(real exp)
}
