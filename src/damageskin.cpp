#include "pch.h"
#include "hook.h"
#include "debug.h"
#include "damageskin.h"
#include "damagelong.h"
#include "clientsocket.h"
#include "wvs/packet.h"
#include "wvs/util.h"
#include "ztl/ztl.h"

#include <atomic>
#include <cstdio>
#include <unordered_map>

// Damage skins: player -> mob damage numbers drawn with a per-character sprite set from Custom.wz, plus
// "unit damage" (1.8K / 2M / 3B / 4T) for skins whose NoCustom/customType is "glUnit".
//
// CAnimationDisplayer::Effect_HP picks its digit sheets in a switch on lColorType. Player -> mob damage is
// colour type 0 (normal or crit); mob -> player is 2 and heals are 1, and those are left alone. We splice
// the two type-0 branches so they load the attacker's skin instead of this+0x170.. sheets. The attacker
// comes from CMob::OnHit, which draws the damage synchronously through CMob::ShowDamage, so the skin is
// scoped to that call; damage drawn anywhere else (mob-damaged packet, DoT, snowball) stays stock.
//
// Unit damage: Effect_HP formats the number with ZXString::Format("%d") and then looks each character up
// as `_itow(c - '0')` in the digit sheet. For a unit skin we rewrite the formatted string with bytes
// ':' ';' '<' '=' '>' (keys "10".."14") and hand Effect_HP a property wrapper that maps those keys onto
// NoCustom/<sheet>/0..4 (. K M B T). Every glUnit skin in Custom.wz carries all five.


// ---------------------------------------------------------------------------------------------------------
// Loaded skins
// ---------------------------------------------------------------------------------------------------------

std::map<int, DamageSkinProp> g_mDamageSkinProp;
std::vector<int> g_vSkinIds;
int g_nActiveSkin = 0;

// Skin used for the Effect_HP / Effect_Miss call in progress. Only non-zero inside CMob::OnHit.
static int g_nRenderSkin = 0;

static void BuildUnitWrappers();

static IWzPropertyPtr GetChildProperty(IWzPropertyPtr pParent, const wchar_t* sName) {
    Ztl_variant_t v;
    if (!pParent || FAILED(pParent->get_item(const_cast<wchar_t*>(sName), &v)) || V_VT(&v) != VT_UNKNOWN) {
        return nullptr;
    }
    IWzPropertyPtr pProp;
    if (FAILED(V_UNKNOWN(&v)->QueryInterface(&pProp))) {
        return nullptr;
    }
    return pProp;
}

static bool g_bLoaded = false;

void LoadDamageSkin() {
    if (g_bLoaded) {
        return;
    }
    g_bLoaded = true;

    IWzPropertyPtr pRoot;
    try {
        Ztl_variant_t vEmpty;
        Ztl_variant_t vRoot;
        if (FAILED(get_rm()->raw_GetObject(const_cast<wchar_t*>(kDamageSkinUOL), vEmpty, vEmpty, &vRoot)) ||
                V_VT(&vRoot) != VT_UNKNOWN || FAILED(V_UNKNOWN(&vRoot)->QueryInterface(&pRoot))) {
            DEBUG_MESSAGE("DamageSkin: %ls not found", kDamageSkinUOL);
            return;
        }

        IEnumVARIANTPtr pEnum = pRoot->_NewEnum;
        while (true) {
            Ztl_variant_t vNext;
            ULONG uCeltFetched = 0;
            if (FAILED(pEnum->Next(1, &vNext, &uCeltFetched)) || uCeltFetched == 0) {
                break;
            }
            if (V_VT(&vNext) != VT_BSTR || !V_BSTR(&vNext)) {
                continue;
            }
            int nID = wcstol(V_BSTR(&vNext), nullptr, 10);
            IWzPropertyPtr pSkin = GetChildProperty(pRoot, V_BSTR(&vNext));
            if (nID <= 0 || !pSkin) {
                continue;
            }

            DamageSkinProp prop;
            prop.pNoRed0 = GetChildProperty(pSkin, L"NoRed0");
            prop.pNoRed1 = GetChildProperty(pSkin, L"NoRed1");
            prop.pNoCri0 = GetChildProperty(pSkin, L"NoCri0");
            prop.pNoCri1 = GetChildProperty(pSkin, L"NoCri1");
            if (!prop.pNoRed0 || !prop.pNoRed1 || !prop.pNoCri0 || !prop.pNoCri1) {
                continue;
            }

            // Other customType values (hangul, hangulUnit, customPool) use different glyph layouts.
            if (IWzPropertyPtr pCustom = GetChildProperty(pSkin, L"NoCustom")) {
                Ztl_variant_t vType;
                if (SUCCEEDED(pCustom->get_item(const_cast<wchar_t*>(L"customType"), &vType)) &&
                        V_VT(&vType) == VT_BSTR && V_BSTR(&vType) && wcscmp(V_BSTR(&vType), L"glUnit") == 0) {
                    prop.pNoCustomRed0 = GetChildProperty(pCustom, L"NoRed0");
                    prop.pNoCustomCri0 = GetChildProperty(pCustom, L"NoCri0");
                    prop.bHasCustom = prop.pNoCustomRed0 && prop.pNoCustomCri0;
                }
            }

            g_mDamageSkinProp[nID] = prop;
            g_vSkinIds.push_back(nID);
        }
    } catch (const _com_error&) {
        DEBUG_MESSAGE("DamageSkin: load failed");
    }

    std::sort(g_vSkinIds.begin(), g_vSkinIds.end());
    BuildUnitWrappers();
    DEBUG_MESSAGE("DamageSkin: loaded %d skins", static_cast<int>(g_vSkinIds.size()));
}


// ---------------------------------------------------------------------------------------------------------
// Effect_HP / Effect_Miss splices
// ---------------------------------------------------------------------------------------------------------

// CAnimationDisplayer digit sheets, loaded by its ctor 0x00435444.
constexpr size_t kOff_NoRed0 = 0x170;
constexpr size_t kOff_NoRed1 = 0x174;
constexpr size_t kOff_NoCri0 = 0x188;
constexpr size_t kOff_NoCri1 = 0x18C;

// _com_ptr_t<IWzProperty>::operator=(IWzProperty*) — unnamed in v83.idb, v83 VA 0x004027BE (ret 4).
static auto SmartPtrAssign = reinterpret_cast<void(__thiscall*)(void*, IWzProperty*)>(0x004027BE);

// Set by the splice helpers, consumed by the Format hook / crit-effect stub later in the same Effect_HP call.
static volatile LONG g_bUnitWrapperActive = 0;
static int g_bSkinCritActive = 0;

static const DamageSkinProp* GetRenderSkin() {
    if (g_nRenderSkin == 0) {
        return nullptr;
    }
    LoadDamageSkin();
    auto it = g_mDamageSkinProp.find(g_nRenderSkin);
    return it == g_mDamageSkinProp.end() ? nullptr : &it->second;
}

// Replaces the stock pair when a skin is active; returns true if it did. pLarge is the first-digit
// sheet ("1"), pSmall the rest ("0"). Unit skins hand out the wrapper for both so every glyph of the run
// (NoCustom only ships the small sheet) renders at one size.
static bool PickSkinSprites(bool bCrit, IWzProperty*& pLarge, IWzProperty*& pSmall, bool& bWrapper) {
    const DamageSkinProp* pSkin = GetRenderSkin();
    if (!pSkin) {
        return false;
    }
    void* pWrap = bCrit ? pSkin->pWrapCrit : pSkin->pWrapNormal;
    if (pSkin->bHasCustom && pWrap) {
        pLarge = pSmall = static_cast<IWzProperty*>(pWrap);
        bWrapper = true;
    } else {
        pLarge = bCrit ? pSkin->pNoCri1.GetInterfacePtr() : pSkin->pNoRed1.GetInterfacePtr();
        pSmall = bCrit ? pSkin->pNoCri0.GetInterfacePtr() : pSkin->pNoRed0.GetInterfacePtr();
    }
    return true;
}

// Stands in for one type-0 branch of Effect_HP's colour switch: assigns the first-digit sheet to the
// IWzPropertyPtr at [ebp-18h] and returns the other sheet, which the merge point assigns to [ebp-20h].
// Any failure falls back to the stock sheets; a null here would make Effect_HP throw E_POINTER.
static IWzProperty* __stdcall EffectHP_Pick(char* pDisp, void* pVar18, bool bCrit) {
    IWzProperty* pLarge = *reinterpret_cast<IWzProperty**>(pDisp + (bCrit ? kOff_NoCri1 : kOff_NoRed1));
    IWzProperty* pSmall = *reinterpret_cast<IWzProperty**>(pDisp + (bCrit ? kOff_NoCri0 : kOff_NoRed0));
    bool bSkin = false;
    bool bWrapper = false;
    __try {
        IWzProperty* pSkinLarge = nullptr;
        IWzProperty* pSkinSmall = nullptr;
        if (PickSkinSprites(bCrit, pSkinLarge, pSkinSmall, bWrapper) && pSkinLarge && pSkinSmall) {
            pLarge = pSkinLarge;
            pSmall = pSkinSmall;
            bSkin = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bSkin = false;
        bWrapper = false;
        pLarge = *reinterpret_cast<IWzProperty**>(pDisp + (bCrit ? kOff_NoCri1 : kOff_NoRed1));
        pSmall = *reinterpret_cast<IWzProperty**>(pDisp + (bCrit ? kOff_NoCri0 : kOff_NoRed0));
    }
    InterlockedExchange(&g_bUnitWrapperActive, bSkin && bWrapper ? 1 : 0);
    g_bSkinCritActive = bCrit && bSkin ? 1 : 0;
    SmartPtrAssign(pVar18, pLarge);
    return pSmall;
}

static IWzProperty* __stdcall EffectHP_PickNormal(char* pDisp, void* pVar18) {
    return EffectHP_Pick(pDisp, pVar18, false);
}

static IWzProperty* __stdcall EffectHP_PickCrit(char* pDisp, void* pVar18) {
    return EffectHP_Pick(pDisp, pVar18, true);
}

// Effect_Miss colour type 0: the skin's NoRed0 (it carries the Miss child) instead of this+0x170.
static IWzProperty* __stdcall EffectMiss_Pick(char* pDisp) {
    IWzProperty* pStock = *reinterpret_cast<IWzProperty**>(pDisp + kOff_NoRed0);
    __try {
        const DamageSkinProp* pSkin = GetRenderSkin();
        if (pSkin && pSkin->pNoRed0) {
            return pSkin->pNoRed0.GetInterfacePtr();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return pStock;
}

// CAnimationDisplayer::Effect_HP — v95 sym, v83 VA 0x00437D0F (ret 14h). esi = this, [ebp-18h] and
// [ebp-20h] are the IWzPropertyPtr digit sheets. Merge point 0x00437DB6 is `lea ecx,[ebp-20h]; call
// 0x004027BE`, which assigns the value pushed by the branch.
static auto EffectHP_MergeRet = 0x00437DB6;

void __declspec(naked) EffectHP_Crit_hook() {
    __asm {
        lea     eax, [ ebp - 0x18 ]
        push    eax
        push    esi
        call    EffectHP_PickCrit
        push    eax
        jmp     [ EffectHP_MergeRet ]
    }
}

void __declspec(naked) EffectHP_Normal_hook() {
    __asm {
        lea     eax, [ ebp - 0x18 ]
        push    eax
        push    esi
        call    EffectHP_PickNormal
        push    eax
        jmp     [ EffectHP_MergeRet ]
    }
}

// Effect_HP 0x00438166 `jz 0x004382F6` skips the crit "effect" overlay when bCriticalAttack == 0 (flags
// from `cmp [ebp+18h],edi`). A skin sheet has no "effect" child, so skip it too when this call used one;
// stock crits keep their glow. edi is 0 at both targets.
static auto EffectHP_CritEffect = 0x0043816C;
static auto EffectHP_CritEffectSkip = 0x004382F6;

void __declspec(naked) EffectHP_CritEffect_hook() {
    __asm {
        jz      skip
        cmp     dword ptr [ g_bSkinCritActive ], 0
        jne     skip
        jmp     [ EffectHP_CritEffect ]
    skip:
        jmp     [ EffectHP_CritEffectSkip ]
    }
}

// CAnimationDisplayer::Effect_Miss — v95 sym, v83 VA 0x00438A21 (ret 0Ch). Replaces the 6-byte
// `mov eax,[esi+170h]` at 0x00438A51; ecx/edx are saved because the original only touched eax.
static auto EffectMiss_Ret = 0x00438A57;

void __declspec(naked) EffectMiss_hook() {
    __asm {
        push    ecx
        push    edx
        push    esi
        call    EffectMiss_Pick
        pop     edx
        pop     ecx
        jmp     [ EffectMiss_Ret ]
    }
}


// ---------------------------------------------------------------------------------------------------------
// Unit damage
// ---------------------------------------------------------------------------------------------------------

// IWzProperty over a unit skin's digit sheet. Keys "0".."9" resolve to damageSkin/<id>/<sheet>/<n>,
// "10".."14" to damageSkin/<id>/NoCustom/<sheet>/<n-10>. Resolved through the resman on each call, so it
// holds no references the cache could invalidate. Owned by g_mDamageSkinProp for the DLL's lifetime.
class CUnitPropertyWrapper : public IWzProperty {
public:
    CUnitPropertyWrapper(int nSkinId, bool bCrit) : m_nSkinId(nSkinId), m_bCrit(bCrit) {
        m_nRef.store(1);
    }

    HRESULT __stdcall QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == __uuidof(IWzSerialize) || riid == __uuidof(IWzProperty)) {
            *ppv = static_cast<IWzProperty*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override {
        return static_cast<ULONG>(++m_nRef);
    }
    ULONG __stdcall Release() override {
        // Clamped at 1 so the engine's releases can never free a wrapper we still hand out.
        long n = --m_nRef;
        if (n <= 0) {
            m_nRef.store(1);
            n = 1;
        }
        return static_cast<ULONG>(n);
    }

    HRESULT __stdcall get_persistentUOL(BSTR*) override {
        return E_NOTIMPL;
    }
    HRESULT __stdcall raw_Serialize(IWzArchive*) override {
        return E_NOTIMPL;
    }

    HRESULT __stdcall get_item(BSTR sPath, VARIANT* pvValue) override {
        // SEH and C++ unwinding can't share a frame (C2712); Resolve owns the Ztl_variant_t locals.
        __try {
            return Resolve(m_nSkinId, m_bCrit, sPath, pvValue);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (pvValue) {
                VariantInit(pvValue);
            }
            return E_FAIL;
        }
    }
    HRESULT __stdcall put_item(BSTR, VARIANT) override {
        return E_NOTIMPL;
    }
    HRESULT __stdcall get__NewEnum(IUnknown**) override {
        return E_NOTIMPL;
    }
    HRESULT __stdcall get_count(unsigned int* p) override {
        if (p) {
            *p = 0;
        }
        return S_OK;
    }
    HRESULT __stdcall raw_Add(BSTR, VARIANT, VARIANT) override {
        return E_NOTIMPL;
    }
    HRESULT __stdcall raw_Remove(BSTR) override {
        return E_NOTIMPL;
    }
    HRESULT __stdcall raw_Import(BSTR) override {
        return E_NOTIMPL;
    }

private:
    static HRESULT Resolve(int nSkinId, bool bCrit, BSTR sPath, VARIANT* pvValue) {
        if (!pvValue) {
            return E_POINTER;
        }
        VariantInit(pvValue);
        if (!sPath) {
            return E_INVALIDARG;
        }

        const wchar_t* sSheet = bCrit ? L"NoCri0" : L"NoRed0";
        wchar_t sUOL[192];
        if (sPath[0] == L'1' && sPath[1] >= L'0' && sPath[1] <= L'4' && sPath[2] == 0) {
            _snwprintf_s(sUOL, _countof(sUOL), _TRUNCATE, L"%ls/%d/NoCustom/%ls/%c",
                    kDamageSkinUOL, nSkinId, sSheet, sPath[1]);
        } else {
            _snwprintf_s(sUOL, _countof(sUOL), _TRUNCATE, L"%ls/%d/%ls/%ls",
                    kDamageSkinUOL, nSkinId, sSheet, static_cast<const wchar_t*>(sPath));
        }

        Ztl_variant_t vEmpty;
        Ztl_variant_t vResult;
        HRESULT hr = get_rm()->raw_GetObject(sUOL, vEmpty, vEmpty, &vResult);
        if (FAILED(hr) || V_VT(&vResult) != VT_UNKNOWN || !V_UNKNOWN(&vResult)) {
            return E_FAIL;
        }
        // Hand vResult's reference to the caller.
        *pvValue = vResult.Detach();
        return S_OK;
    }

    int m_nSkinId;
    bool m_bCrit;
    std::atomic<long> m_nRef;
};

static void BuildUnitWrappers() {
    for (auto& [nID, prop] : g_mDamageSkinProp) {
        if (prop.bHasCustom && !prop.pWrapNormal) {
            prop.pWrapNormal = static_cast<IWzProperty*>(new CUnitPropertyWrapper(nID, false));
            prop.pWrapCrit = static_cast<IWzProperty*>(new CUnitPropertyWrapper(nID, true));
        }
    }
}

// digits -> '0'..'9', '.' -> ':' (key "10"), K -> ';' ("11"), M -> '<' ("12"), B -> '=' ("13"),
// T -> '>' ("14"). Keeps one decimal when it is not zero: 1834 -> "1.8K", 2000000 -> "2M".
// T is the top unit, so Long.MAX_VALUE prints as "9223372T". Effect_HP hands us an int; a line
// that did not fit arrives as a damagelong.h tag and EffectHP_Format_hook resolves it first.
static void FormatUnitDamage(long long nDamage, char* sOut, size_t uCap) {
    // unsigned magnitude: -LLONG_MIN would overflow
    unsigned long long n = nDamage < 0 ? 0ULL - static_cast<unsigned long long>(nDamage) : nDamage;
    char cUnit = ';';
    unsigned long long nDiv = 1000;
    if (n >= 1000000000000ULL) {
        cUnit = '>';
        nDiv = 1000000000000ULL;
    } else if (n >= 1000000000ULL) {
        cUnit = '=';
        nDiv = 1000000000LL;
    } else if (n >= 1000000ULL) {
        cUnit = '<';
        nDiv = 1000000ULL;
    }
    unsigned long long nWhole = n / nDiv;
    unsigned long long nTenths = (n % nDiv) / (nDiv / 10);
    if (nTenths == 0) {
        _snprintf_s(sOut, uCap, _TRUNCATE, "%llu%c", nWhole, cUnit);
    } else {
        _snprintf_s(sOut, uCap, _TRUNCATE, "%llu:%llu%c", nWhole, nTenths, cUnit);
    }
}

// ZXString<char>::Format — v95 sym, v83 VA 0x00445B4B (cdecl, varargs).
static auto ZXString_Format = reinterpret_cast<ZXString<char>*(__cdecl*)(ZXString<char>*, const char*, ...)>(0x00445B4B);

// Replaces Effect_HP's `call ZXString<char>::Format(&s, "%d", nDamage)` at 0x00437DFB. Rewrites the result
// only when this Effect_HP call installed a unit wrapper; other colour types reach this call too, and
// ':'/';' against a stock sheet would queue invalid canvases into the animation layer.
static ZXString<char>* __cdecl EffectHP_Format_hook(ZXString<char>* pResult, const char* sFormat, int nDamage) {
    LONG bWrapper = InterlockedExchange(&g_bUnitWrapperActive, 0);
    ZXString_Format(pResult, sFormat, nDamage);
    bool bTag = DamageLong_IsTag(nDamage);
    long long nReal = bTag ? DamageLong_Resolve(nDamage) : nDamage;
    if (bWrapper && nReal >= 1000) {
        char sUnit[32];
        FormatUnitDamage(nReal, sUnit, sizeof(sUnit));
        *pResult = sUnit;
    } else if (bTag) {
        // stock digit sheet: the real decimal instead of the tag's 2147xxxxxx
        char sReal[32];
        _snprintf_s(sReal, sizeof(sReal), _TRUNCATE, "%lld", nReal);
        *pResult = sReal;
    }
    return pResult;
}


// ---------------------------------------------------------------------------------------------------------
// Attacker attribution
// ---------------------------------------------------------------------------------------------------------

std::vector<DamageSkinCatalogEntry> g_vShopCatalog;
std::vector<int> g_vOwnedSkins;

static std::unordered_map<int, int> g_mCharIdToSkin;

void ClearDamageSkinBroadcasts() {
    g_mCharIdToSkin.clear();
}

// CUserLocal singleton 0x00BEBF98; dwCharacterId at +0x11A8 (CUser::CUser, getter 0x007A6E53).
static int GetLocalCharId() {
    char* pUser = *reinterpret_cast<char**>(0x00BEBF98);
    return pUser ? *reinterpret_cast<int*>(pUser + 0x11A8) : 0;
}

// CMob::OnHit — v95 sym, v83 VA 0x00668B83 (ret 34h: 13 stack args; v95 has 14). arg1 is the attacker's
// character id (the local id for local summons). Draws the damage through CMob::ShowDamage (0x00668E0D).
static auto CMob__OnHit = reinterpret_cast<void(__thiscall*)(void*, unsigned int, int, int, int, int, int, int, int, int, int, int, int, int)>(0x00668B83);

int DamageSkin_BeginAttacker(int nAttacker) {
    int nSaved = g_nRenderSkin;
    if (nAttacker != 0 && nAttacker == GetLocalCharId()) {
        g_nRenderSkin = g_nActiveSkin;
    } else {
        auto it = g_mCharIdToSkin.find(nAttacker);
        g_nRenderSkin = it == g_mCharIdToSkin.end() ? 0 : it->second;
    }
    return nSaved;
}

void DamageSkin_End(int nPrevious) {
    g_nRenderSkin = nPrevious;
}

static void __fastcall CMob__OnHit_hook(void* pThis, void* _EDX, unsigned int dwAttackerId, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9, int a10, int a11, int a12, int a13) {
    int nSaved = DamageSkin_BeginAttacker(static_cast<int>(dwAttackerId));
    CMob__OnHit(pThis, dwAttackerId, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
    DamageSkin_End(nSaved);
}


// ---------------------------------------------------------------------------------------------------------
// Network — must match Server PacketCreator.damageSkin* and the DamageSkin*Handler classes
// ---------------------------------------------------------------------------------------------------------

constexpr unsigned short CP_DAMAGE_SKIN_APPLY = 0x110;    // Server RecvOpcode.DAMAGE_SKIN_APPLY
constexpr unsigned short CP_DAMAGE_SKIN_PURCHASE = 0x111; // Server RecvOpcode.DAMAGE_SKIN_PURCHASE

void Send_DamageSkinApply(int nSkinId) {
    COutPacket oPacket(CP_DAMAGE_SKIN_APPLY);
    oPacket.Encode4(nSkinId);
    SendClientPacket(oPacket);
}

void Send_DamageSkinPurchase(int nSkinId) {
    COutPacket oPacket(CP_DAMAGE_SKIN_PURCHASE);
    oPacket.Encode4(nSkinId);
    SendClientPacket(oPacket);
}

// CUtilDlg::Notice — v95 sym, v83 VA 0x009929DD (cdecl: msg by value, sound, ZRef<CDialog>*, int, int).
static auto CUtilDlg__Notice = reinterpret_cast<int(__cdecl*)(ZXString<char>, const wchar_t*, void*, int, int)>(0x009929DD);

namespace damageskin {

// [short n] { [int skinId] [long price] } x n
static void OnCatalog(CInPacket* pPacket) {
    int nCount = pPacket->Decode<unsigned short>();
    g_vShopCatalog.clear();
    for (int i = 0; i < nCount; ++i) {
        DamageSkinCatalogEntry entry;
        entry.nID = pPacket->Decode<int>();
        entry.llPrice = pPacket->Decode<long long>();
        g_vShopCatalog.push_back(entry);
    }
    RefreshDamageSkinPicker();
}

// [int activeSkinId] [short n] { [int skinId] } x n — sent at login, so load the 77 MB skin tree now
// rather than on the first hit.
static void OnInventory(CInPacket* pPacket) {
    g_nActiveSkin = pPacket->Decode<int>();
    int nCount = pPacket->Decode<unsigned short>();
    g_vOwnedSkins.clear();
    for (int i = 0; i < nCount; ++i) {
        g_vOwnedSkins.push_back(pPacket->Decode<int>());
    }
    LoadDamageSkin();
    RefreshDamageSkinPicker();
}

// [byte op: 1 apply, 2 purchase] [byte ok] [int skinId] [long meso]
static void OnResult(CInPacket* pPacket) {
    int nOp = pPacket->Decode<unsigned char>();
    int bOk = pPacket->Decode<unsigned char>();
    int nSkinId = pPacket->Decode<int>();
    pPacket->Decode<long long>(); // balance; the stat packet updates the wallet
    if (!bOk) {
        return;
    }
    if (nOp == 1) {
        g_nActiveSkin = nSkinId;
        CUtilDlg__Notice(ZXString<char>("Damage skin has been applied."), nullptr, nullptr, 0, 0);
    } else if (nOp == 2) {
        if (std::find(g_vOwnedSkins.begin(), g_vOwnedSkins.end(), nSkinId) == g_vOwnedSkins.end()) {
            g_vOwnedSkins.push_back(nSkinId);
        }
    }
    RefreshDamageSkinPicker();
}

// [int charId] [int skinId]
static void OnBroadcast(CInPacket* pPacket) {
    int nCharId = pPacket->Decode<int>();
    int nSkinId = pPacket->Decode<int>();
    if (nSkinId == 0) {
        g_mCharIdToSkin.erase(nCharId);
    } else {
        g_mCharIdToSkin[nCharId] = nSkinId;
    }
    if (nCharId == GetLocalCharId()) {
        g_nActiveSkin = nSkinId;
    }
}

void HandlePacket(CInPacket* pPacket) {
    unsigned short nType = pPacket->Decode<unsigned short>();
    switch (nType) {
    case LP_CATALOG:
        OnCatalog(pPacket);
        break;
    case LP_INVENTORY:
        OnInventory(pPacket);
        break;
    case LP_RESULT:
        OnResult(pPacket);
        break;
    case LP_BROADCAST:
        OnBroadcast(pPacket);
        break;
    }
}

} // namespace damageskin


void AttachDamageSkinMod() {
    PatchJmp(0x00437D8C, &EffectHP_Crit_hook);            // Effect_HP colour 0 crit branch: push [esi+18Ch] (6 B)
    PatchJmp(0x00437DA2, &EffectHP_Normal_hook);          // Effect_HP colour 0 normal branch: push [esi+174h] (6 B)
    PatchJmp(0x00438166, &EffectHP_CritEffect_hook);      // Effect_HP jz 004382F6 (6 B): crit "effect" overlay
    Patch1(0x00438166 + 5, 0x90);                         // Effect_HP: 6th byte of that jz
    PatchCall(0x00437DFB, &EffectHP_Format_hook);         // Effect_HP call ZXString<char>::Format("%d") -> unit damage
    PatchJmp(0x00438A51, &EffectMiss_hook);               // Effect_Miss colour 0: mov eax,[esi+170h] (6 B)
    Patch1(0x00438A51 + 5, 0x90);                         // Effect_Miss: 6th byte of that mov

    // Effect_HP scratch canvas: 57 rows above the anchor clip tall skin glyphs. The three constants move
    // together so the number keeps its screen position: 100 rows above the origin, 27 below.
    Patch1(0x0043805F, 0x7F); // Effect_HP push 39h -> 7Fh: canvas Create height (57 -> 127)
    Patch1(0x00438355, 0x64); // Effect_HP push 39h -> 64h: Copy y anchor inside the canvas (57 -> 100)
    Patch1(0x004383F6, 0xA6); // Effect_HP add eax,-2Fh -> -5Ah: layer top offset (-47 -> -90)

    ATTACH_HOOK(CMob__OnHit, CMob__OnHit_hook);
}
