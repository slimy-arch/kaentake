#include "pch.h"
#include "hook.h"
#include "wvs/util.h"
#include "ztl/ztl.h"


class CWzCanvas : public IWzCanvas {
public:
    typedef HRESULT(__stdcall* raw_Serialize_t)(IWzCanvas*, IWzArchive*);
    inline static raw_Serialize_t raw_Serialize_orig;

    HRESULT __stdcall raw_Serialize_hook(IWzArchive* pArchive) {
        HRESULT hr = raw_Serialize_orig(this, pArchive);
        if (FAILED(hr)) {
            return hr;
        }
        Ztl_variant_t vInlink = this->property->item[L"_inlink"];
        if (V_VT(&vInlink) == VT_BSTR) {
            ZXString<wchar_t> sFilePath(pArchive->absoluteUOL);
            // keep "<...>.img/"; a UOL without it would be truncated to garbage (or past its end)
            int nImg = sFilePath.Find(L".img");
            if (nImg < 0 || nImg + 5 > sFilePath.GetLength()) {
                return hr;
            }
            sFilePath.ReleaseBuffer(nImg + 5);
            sFilePath.Cat(V_BSTR(&vInlink));
            this->property->item[L"_inlink"] = static_cast<const wchar_t*>(sFilePath);
        }
        return hr;
    }
};

// Raw tile (0,0), or null. The raw getter avoids the wrapper's throw on a canvas with no tiles.
static IWzRawCanvasPtr GetRawTile(IWzCanvas* pCanvas) {
    IWzRawCanvas* pRaw = nullptr;
    if (FAILED(pCanvas->get_rawCanvas(0, 0, &pRaw))) {
        return nullptr;
    }
    return IWzRawCanvasPtr(pRaw, false); // getter already AddRef'd
}

void HandleLinkProperty(IWzCanvasPtr pCanvas) {
    // Runs on every canvas fetch, so the key strings are allocated once. Never freed: a static
    // Ztl_bstr_t destructor would run after the client's allocator is gone.
    static const Ztl_bstr_t* asLinkProperty[] = {
        new Ztl_bstr_t(L"_inlink"),
        new Ztl_bstr_t(L"_outlink"),
        new Ztl_bstr_t(L"source"),
    };
    size_t nLinkProperty = sizeof(asLinkProperty) / sizeof(asLinkProperty[0]);
    IWzPropertyPtr pProperty = pCanvas->property;
    for (size_t i = 0; i < nLinkProperty; ++i) {
        Ztl_variant_t vLink = pProperty->item[*asLinkProperty[i]];
        if (V_VT(&vLink) != VT_BSTR) {
            continue;
        }

        // Try resolving source canvas
        IWzCanvasPtr pSource;
        IUnknownPtr pUnknown = get_rm()->GetObjectA(V_BSTR(&vLink)).GetUnknown();
        if (!pUnknown || FAILED(pUnknown->QueryInterface(&pSource))) {
            DEBUG_MESSAGE("Could not resolve linked canvas %ls=\"%ls\"", static_cast<const wchar_t*>(*asLinkProperty[i]), V_BSTR(&vLink));
            continue;
        }

        // Already resolved on an earlier fetch: the link property stays on the canvas, so without
        // this every fetch re-ran Create, allocating and freeing a full-size pixel buffer.
        IWzRawCanvasPtr pSourceRaw = GetRawTile(pSource);
        if (pSourceRaw && GetRawTile(pCanvas).GetInterfacePtr() == pSourceRaw.GetInterfacePtr()) {
            break;
        }

        // Create target canvas
        int nWidth, nHeight, nFormat, nMagLevel;
        pSource->GetSnapshot(&nWidth, &nHeight, nullptr, nullptr, (CANVAS_PIXFORMAT*)&nFormat, &nMagLevel);
        pCanvas->Create(nWidth, nHeight, nMagLevel, nFormat);
        pCanvas->AddRawCanvas(0, 0, pSourceRaw ? pSourceRaw : pSource->rawCanvas[0][0]);

        // Set target origin
        IWzVector2DPtr pOrigin = pProperty->item[L"origin"].GetUnknown();
        pCanvas->cx = pOrigin->x;
        pCanvas->cy = pOrigin->y;
        break;
    }
}

static auto get_unknown_orig = reinterpret_cast<IUnknownPtr*(__cdecl*)(IUnknownPtr*, Ztl_variant_t&)>(0x00414ADA);
// No longer detoured: get_unknown resolves through Ztl_variant_t::GetUnknown (0x00414AF4, and 0x00414BC0
// for UOLs), which is itself detoured, so hooking both ran HandleLinkProperty twice per fetch. Kept as the DLL-side
// entry point for util.h get_unknown().
IUnknownPtr* __cdecl get_unknown_hook(IUnknownPtr* result, Ztl_variant_t& v) {
    return get_unknown_orig(result, v);
}

static auto Ztl_variant_t__GetUnknown = reinterpret_cast<IUnknown*(__thiscall*)(Ztl_variant_t*, bool, bool)>(0x004032B2);
IUnknown* __fastcall Ztl_variant_t__GetUnknown_hook(Ztl_variant_t* pThis, void* _EDX, bool fAddRef, bool fTryChangeType) {
    IUnknownPtr result = pThis->GetUnknown(fAddRef, fTryChangeType);
    IWzCanvasPtr pCanvas;
    if (SUCCEEDED(result.QueryInterface(__uuidof(IWzCanvas), &pCanvas))) {
        HandleLinkProperty(pCanvas);
    }
    return result;
}


void AttachClientInlink() {
    CWzCanvas::raw_Serialize_orig = reinterpret_cast<CWzCanvas::raw_Serialize_t>(GetAddressByPattern("CANVAS.DLL", "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 EC 6C"));
    ATTACH_HOOK(CWzCanvas::raw_Serialize_orig, CWzCanvas::raw_Serialize_hook);
    ATTACH_HOOK(Ztl_variant_t__GetUnknown, Ztl_variant_t__GetUnknown_hook); // also reached from inside get_unknown (0x00414ADA), so this one hook covers both paths
}