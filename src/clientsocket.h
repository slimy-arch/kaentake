#pragma once
#include "wvs/packet.h"

// Sends through the live CClientSocket (clientsocket.cpp). No-op before the socket exists.
void SendClientPacket(const COutPacket& oPacket);
