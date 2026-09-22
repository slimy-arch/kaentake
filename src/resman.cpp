#include "pch.h"
#include "hook.h"
#include "debug.h"
#include "wvs/wvsapp.h"
#include "wvs/util.h"
#include "ztl/ztl.h"
#include <algorithm>
#include <vector>
#include <tuple>


static IWzNameSpacePtr g_pCustomNameSpace;
static std::vector<Ztl_bstr_t> g_vecOverrides;

class IWzNameSpaceImpl {
public:
    typedef HRESULT(__stdcall* raw__OnGetLocalObject_t)(IWzNameSpaceImpl*, int, BSTR, int*, VARIANT*);
    inline static raw__OnGetLocalObject_t raw__OnGetLocalObject_orig;

    HRESULT __stdcall raw__OnGetLocalObject_hook(int nIndex, BSTR sPath, int* pnPathUsed, VARIANT* pvRet) {
        HRESULT hr = raw__OnGetLocalObject_orig(this, nIndex, sPath, pnPathUsed, pvRet);
        if (SUCCEEDED(hr)) {
            return hr;
        }
        if (!std::binary_search(g_vecOverrides.begin(), g_vecOverrides.end(), Ztl_bstr_t(sPath))) {
            return hr;
        }
        return g_pCustomNameSpace->raw__OnGetLocalObject(nIndex, sPath, pnPathUsed, pvRet);
    }
};

class CWzProperty : public IWzProperty {
public:
    typedef HRESULT(__stdcall* raw_Serialize_t)(CWzProperty*, IWzArchive*);
    inline static raw_Serialize_t raw_Serialize_orig;

    HRESULT __stdcall raw_Serialize_hook(IWzArchive* pArchive) {
        HRESULT hr = raw_Serialize_orig(this, pArchive);
        if (FAILED(hr)) {
            return hr;
        }
        if (!std::binary_search(g_vecOverrides.begin(), g_vecOverrides.end(), pArchive->absoluteUOL)) {
            return hr;
        }
        IWzPropertyPtr pProperty = get_rm()->GetObjectA(L"Custom/" + pArchive->absoluteUOL).GetUnknown();
        IEnumVARIANTPtr pEnum = pProperty->_NewEnum;
        while (true) {
            Ztl_variant_t vNext;
            ULONG uCeltFetched;
            if (FAILED(pEnum->Next(1, &vNext, &uCeltFetched)) || uCeltFetched == 0) {
                break;
            }
            Ztl_bstr_t sNext = V_BSTR(&vNext);
            IUnknownPtr pUnk = pProperty->item[sNext].GetUnknown();
            IWzPropertyPtr pSub;
            if (!pUnk || FAILED(pUnk->QueryInterface(&pSub))) {
                this->Add(sNext, pProperty->item[sNext], false);
            }
        }
        return S_OK;
    }
};


static bool PathIsDirectory(const char* sPath) {
    DWORD dwAttr = GetFileAttributesA(sPath);
    return dwAttr != INVALID_FILE_ATTRIBUTES && (dwAttr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool PathIsFile(const char* sPath) {
    DWORD dwAttr = GetFileAttributesA(sPath);
    return dwAttr != INVALID_FILE_ATTRIBUTES && !(dwAttr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool JoinPath(char* sOut, size_t uCap, const char* sDir, const char* sName) {
    return SUCCEEDED(StringCbCopyA(sOut, uCap, sDir)) &&
        SUCCEEDED(StringCbCatA(sOut, uCap, "/")) &&
        SUCCEEDED(StringCbCatA(sOut, uCap, sName));
}

// Full img mode only after Base.wz has been dumped (zmap.img at Data root).
// A half-exported Data/UI folder must not skip the packed .wz archives.
static bool HasFullImgDataTree(const char* sDataPath) {
    char sProbe[MAX_PATH];
    return JoinPath(sProbe, sizeof(sProbe), sDataPath, "zmap.img") && PathIsFile(sProbe);
}

static bool MountFileSystemAtRoot(const char* sPath) {
    try {
        IWzFileSystemPtr pFileSystem;
        PcCreateObject<IWzFileSystemPtr>(L"NameSpace#FileSystem", pFileSystem, nullptr);
        pFileSystem->Init(sPath);
        get_root()->Mount(L"/", pFileSystem, 0);
        return true;
    } catch (const _com_error&) {
        DEBUG_MESSAGE("Failed to mount filesystem: %s", sPath);
        return false;
    }
}

// Port of ClientImageLoader's InitializeResMan: skip the .wz packages and serve
// loose .img files from <game>/Data instead. The game dir is still mounted so
// List.wz and Custom.wz remain reachable.
static bool InitializeResManFromImg(const char* sGameDir, const char* sDataPath) {
    try {
        PcCreateObject<IWzResManPtr>(L"ResMan", get_rm(), nullptr);
        get_rm()->SetResManParam(
            static_cast<RESMAN_PARAM>(RC_AUTO_REPARSE | RC_AUTO_SERIALIZE),
            -1,
            -1);

        PcCreateObject<IWzNameSpacePtr>(L"NameSpace", get_root(), nullptr);
        PcSetRootNameSpace(get_root());

        if (!MountFileSystemAtRoot(sGameDir)) {
            return false;
        }
        if (!MountFileSystemAtRoot(sDataPath)) {
            return false;
        }
        DEBUG_MESSAGE("ResMan: loading .img from %s", sDataPath);
        return true;
    } catch (const _com_error&) {
        DEBUG_MESSAGE("ResMan: img init failed, falling back to .wz");
        return false;
    }
}

static bool OverlayImgData(const char* sDataPath) {
    if (!MountFileSystemAtRoot(sDataPath)) {
        return false;
    }
    try {
        get_rm()->SetResManParam(
            static_cast<RESMAN_PARAM>(RC_AUTO_REPARSE | RC_AUTO_SERIALIZE),
            -1,
            -1);
    } catch (const _com_error&) {
        DEBUG_MESSAGE("ResMan: could not enable AUTO_REPARSE for img overlay");
    }
    DEBUG_MESSAGE("ResMan: overlaying .img from %s", sDataPath);
    return true;
}

static void MountCustomWz(const char* sGameDir) {
    // add custom namespace to root
    IWzWritableNameSpacePtr pWritableRoot;
    if (FAILED(get_root()->QueryInterface(&pWritableRoot))) {
        ErrorMessage("Failed to cast root namespace");
        return;
    }
    IWzNameSpacePtr pNameSpace;
    PcCreateObject<IWzNameSpacePtr>(L"NameSpace", pNameSpace, nullptr);
    Ztl_variant_t vResult;
    pWritableRoot->AddObject(L"Custom", static_cast<IUnknown*>(pNameSpace), &vResult);
    g_pCustomNameSpace = vResult.GetUnknown();

    // load Custom.wz from file system
    IWzFileSystemPtr fs;
    PcCreateObject<IWzFileSystemPtr>(L"NameSpace#FileSystem", fs, nullptr);
    fs->Init(sGameDir);

    IWzPackagePtr pPackage;
    PcCreateObject<IWzPackagePtr>(L"NameSpace#Package", pPackage, nullptr);
    IWzSeekableArchivePtr pArchive = fs->item[L"Custom.wz"].GetUnknown();
    pPackage->Init(L"83", L"Custom", pArchive);
    g_pCustomNameSpace->Mount(L"/", pPackage, 1);

    // iterate custom namespace
    std::vector<std::tuple<Ztl_bstr_t, IEnumVARIANTPtr>> stack;
    stack.emplace_back(L"", g_pCustomNameSpace->_NewEnum);
    while (!stack.empty()) {
        auto [sPath, pEnum] = stack.back();
        stack.pop_back();

        while (true) {
            Ztl_variant_t vNext;
            ULONG uCeltFetched;
            if (FAILED(pEnum->Next(1, &vNext, &uCeltFetched)) || uCeltFetched == 0) {
                break;
            }
            Ztl_bstr_t sUOL = (sPath.length() > 0 ? sPath + L"/" : L"") + V_BSTR(&vNext);
            Ztl_variant_t vObj = get_rm()->GetObjectA(L"Custom/" + sUOL);
            IUnknownPtr pUnk = vObj.GetUnknown();
            if (pUnk) {
                IWzNameSpacePtr pSub;
                if (SUCCEEDED(pUnk->QueryInterface(&pSub))) {
                    stack.emplace_back(sUOL, pSub->_NewEnum);
                    continue;
                }
                IWzPropertyPtr pProp;
                if (SUCCEEDED(pUnk->QueryInterface(&pProp))) {
                    stack.emplace_back(sUOL, pProp->_NewEnum);
                }
            }
            g_vecOverrides.push_back(sUOL);
        }
    }
    std::sort(g_vecOverrides.begin(), g_vecOverrides.end()); // uses operator<

    // NameSpace.dll - try resolving from g_pCustomNameSpace
    IWzNameSpaceImpl::raw__OnGetLocalObject_orig = static_cast<IWzNameSpaceImpl::raw__OnGetLocalObject_t>(GetAddressByPattern("NAMESPACE.DLL", "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 81 EC 80"));
    ATTACH_HOOK(IWzNameSpaceImpl::raw__OnGetLocalObject_orig, IWzNameSpaceImpl::raw__OnGetLocalObject_hook);

    // PCOM.dll - patch CWzProperty objects during serialization
    CWzProperty::raw_Serialize_orig = static_cast<CWzProperty::raw_Serialize_t>(GetAddressByPattern("PCOM.DLL", "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 EC 68"));
    ATTACH_HOOK(CWzProperty::raw_Serialize_orig, CWzProperty::raw_Serialize_hook);
}


void CWvsApp::InitializeResMan_hook() {
    DEBUG_MESSAGE("CWvsApp::InitializeResMan");
    LogMessage("ResMan: begin");

    char sStartPath[MAX_PATH];
    GetModuleFileNameA(nullptr, sStartPath, MAX_PATH);
    Dir_BackSlashToSlash(sStartPath);
    Dir_upDir(sStartPath);

    char sDataPath[MAX_PATH];
    const bool bDataDir = JoinPath(sDataPath, sizeof(sDataPath), sStartPath, "Data") &&
        PathIsDirectory(sDataPath);
    const bool bFullImg = bDataDir &&
        HasFullImgDataTree(sDataPath) &&
        InitializeResManFromImg(sStartPath, sDataPath);

    if (!bFullImg) {
        get_rm() = nullptr;
        get_root() = nullptr;
        CWvsApp::InitializeResMan(this);
        if (bDataDir) {
            OverlayImgData(sDataPath);
        }
    }

    MountCustomWz(sStartPath);
    LogMessage("ResMan: done (img=%d)", bFullImg ? 1 : 0);
}

void CWvsApp::CleanUp_hook() {
    CWvsApp::CleanUp(this);
    g_pCustomNameSpace = nullptr;
}


void AttachResManMod() {
    ATTACH_HOOK(CWvsApp::InitializeResMan, CWvsApp::InitializeResMan_hook);
    ATTACH_HOOK(CWvsApp::CleanUp, CWvsApp::CleanUp_hook);
}
