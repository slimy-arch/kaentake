#pragma once

// ============================================================
// cashshopwnd.h — shared surface of the standalone Cash Shop window.
//
// See cashshopwnd.cpp for the design. Only the opcode pair and the three entry
// points clientsocket.cpp / bypass.cpp need are declared here, which
// is the same split setitem.h and equiproll.h use.
// ============================================================

class CInPacket;

// Custom opcode pair, this project 0x372x block. The block was full through 0x372F
// (BAG_WINDOW 0x3724/5, SET_ITEM 0x3726/7, EQUIP_ROLL 0x3728/9, GACHAPON 0x372A,
// BEAUTY 0x372C/D, WEAPON_TINT 0x372E/F), so this is the next free pair under the
// even = request / odd = reply convention. 0x372B is free but odd and so cannot be
// a CP. Server halves are net.opcodes.RecvOpcode.CASHSHOP_WINDOW_ACTION /
// SendOpcode.CASHSHOP_WINDOW_SYNC — equal only by convention, nothing enforces it,
// so change both sides or neither.
static constexpr unsigned short kCashShopActionOpcode = 0x3730;   // client -> server
static constexpr unsigned short kCashShopSyncOpcode   = 0x3731;   // server -> client

// Routed from clientsocket.cpp. Runs on the RECEIVE thread: it only fills the
// catalog under a mutex and raises a flag, it never touches the window.
void CashShopWnd_HandleSync(CInPacket* pPacket);

// Per-frame driver from CWvsApp::CallUpdate_hook (bypass.cpp). Creating a CWnd is a
// main-thread job, so the server's "open" arrives as a flag and is acted on here.
void CashShopWnd_Tick();

// Routed from inventorynx.cpp's INVENTORY_CASH handler: updates the NX readout while the window is open.
void CashShopWnd_SetNxCredit(long long nNxCredit);
