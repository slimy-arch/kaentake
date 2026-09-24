#include "pch.h"
#include "hook.h"
#include "clientsocket.h"
#include "uiDamageRank.h"
#include "damageskin.h"
#include "wvs/packet.h"

// Central inbound dispatcher for custom server -> client opcodes. Add new custom packets here rather
// than stacking more detours on CClientSocket::ProcessPacket.

constexpr unsigned short LP_WORLD_MAP_PLAYERS = 0x178; // Server SendOpcode.WORLD_MAP_PLAYERS
constexpr unsigned short LP_DAMAGE_RANK = 0x3714;      // Server SendOpcode.DPT_TRACKER

namespace players {
void HandleResponse(CInPacket* pPacket); // worldmapinfo.cpp
}

// CClientSocket::ProcessPacket — v95 sym, v83 VA 0x004965F1 (ret 4). The opcode sits at the packet's
// current offset (the original starts with Decode2), which is not necessarily 0.
static auto CClientSocket__ProcessPacket = reinterpret_cast<void(__thiscall*)(void*, CInPacket*)>(0x004965F1);

static void __fastcall CClientSocket__ProcessPacket_hook(void* pThis, void* _EDX, CInPacket* pPacket) {
    switch (pPacket->Peek2()) {
    case LP_WORLD_MAP_PLAYERS:
        players::HandleResponse(pPacket);
        return;
    case LP_DAMAGE_RANK:
        CUIDamageRank::GetInstance().HandleTrackerPacket(pPacket);
        return;
    case damageskin::LP_CATALOG:
    case damageskin::LP_INVENTORY:
    case damageskin::LP_RESULT:
    case damageskin::LP_BROADCAST:
        damageskin::HandlePacket(pPacket);
        return;
    }
    CClientSocket__ProcessPacket(pThis, pPacket);
}

// CClientSocket::SendPacket — v95 sym, v83 VA 0x0049637B (ret 4); instance TSingleton<CClientSocket> 0x00BE7914.
void SendClientPacket(const COutPacket& oPacket) {
    void* pSocket = *reinterpret_cast<void**>(0x00BE7914);
    if (!pSocket) {
        return;
    }
    reinterpret_cast<void(__thiscall*)(void*, const COutPacket&)>(0x0049637B)(pSocket, oPacket);
}

void AttachClientSocketMod() {
    ATTACH_HOOK(CClientSocket__ProcessPacket, CClientSocket__ProcessPacket_hook);
}
