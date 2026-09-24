#pragma once
#include "hook.h"
#include "wvs/stage.h"
#include "wvs/wnd.h"
#include "ztl/ztl.h"


class CMapLoadable : public CStage {
public:
    MEMBER_AT(IWzPropertyPtr, 0x2C, m_pPropFieldInfo)
    MEMBER_AT(RECT, 0xF0, m_rcViewRange)
    MEMBER_HOOK(void, 0x00641EF1, RestoreViewRange) // resolution.cpp
};


class CField : public CMapLoadable {
public:
    MEMBER_AT(ZRef<CWnd>, 0x1C8, m_pClock) // ZRef<CClock>
    // CField::OnKey — v95 sym, v83 VA 0x00529968 (ret 8)
    inline static auto OnKey = reinterpret_cast<void(__thiscall*)(CField*, unsigned int, int)>(0x00529968);
};


inline CField* get_field() {
    return reinterpret_cast<CField*(__cdecl*)()>(0x00437A0C)();
}