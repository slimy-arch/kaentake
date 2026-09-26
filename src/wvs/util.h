#pragma once
#include "ztl/ztl.h"

inline IWzGr2DPtr& get_gr() {
    return *reinterpret_cast<IWzGr2DPtr*>(0x00BF14EC);
}

inline IWzResManPtr& get_rm() {
    return *reinterpret_cast<IWzResManPtr*>(0x00BF14E8);
}

inline IWzNameSpacePtr& get_root() {
    return *reinterpret_cast<IWzNameSpacePtr*>(0x00BF14E0);
}

inline int get_int32(Ztl_variant_t& v, int nDefault) {
    Ztl_variant_t vInt;
    if (V_VT(&v) == VT_EMPTY || V_VT(&v) == VT_ERROR || FAILED(ZComAPI::ZComVariantChangeType(&vInt, &v, 0, VT_I4))) {
        return nDefault;
    } else {
        return V_I4(&vInt);
    }
}

// rvalue form, so get_int32(get_object_or_empty(...), n) compiles without MSVC's permissive mode
inline int get_int32(Ztl_variant_t&& v, int nDefault) {
    return get_int32(v, nDefault);
}

// resman lookup that returns an empty variant instead of throwing when the UOL is missing
inline Ztl_variant_t get_object_or_empty(const wchar_t* sUOL) {
    try {
        if (!sUOL) {
            return Ztl_variant_t();
        }
        IWzResManPtr& rm = get_rm();
        if (!rm) {
            return Ztl_variant_t();
        }
        return rm->GetObjectA(sUOL);
    } catch (...) {
        return Ztl_variant_t();
    }
}

inline Ztl_variant_t get_item_or_empty(IWzProperty* pProp, const wchar_t* sName) {
    try {
        if (!pProp || !sName) {
            return Ztl_variant_t();
        }
        return pProp->item[sName];
    } catch (...) {
        return Ztl_variant_t();
    }
}

inline Ztl_variant_t get_item_or_empty(const IWzPropertyPtr& pProp, const wchar_t* sName) {
    IWzProperty* raw = pProp;
    return get_item_or_empty(raw, sName);
}

// implementation in resolution.cpp
int get_screen_width();
int get_screen_height();
int get_adjust_cy();

// implementation in inlink.cpp
IUnknownPtr* __cdecl get_unknown_hook(IUnknownPtr* result, Ztl_variant_t& v);
inline IUnknownPtr get_unknown(Ztl_variant_t& v) {
    IUnknownPtr pUnk;
    get_unknown_hook(std::addressof(pUnk), v);
    return pUnk;
}