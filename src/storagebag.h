#pragma once

// Storage Bag: Ore / Scroll / Chair / Cash, one tabbed window over four per-account server bags.
// Ported from Kaentake/kaentake-main/implemented/storagebag. Opcodes are the BAG_WINDOW pair of the
// 0x372x block documented in cashshopwnd.h. Even = client request, odd = server snapshot. The server
// halves are net.opcodes.RecvOpcode.BAG_WINDOW / SendOpcode.BAG_WINDOW, so change both sides or neither.

class CInPacket;

static constexpr unsigned short kStorageBagRequestOpcode = 0x3724;  // client -> server
static constexpr unsigned short kStorageBagSnapshotOpcode = 0x3725; // server -> client

// Key Config shortcut (type 4 id 56, damagerankinput.cpp) and the inventory BAG button.
void StorageBag_Toggle();

// Esc: cashshopwnd.cpp's CWvsContext::TryCloseUI hook asks this first. True = pWnd was the bag
// window and it is now closed.
bool StorageBag_TryCloseUI(void* pWnd);

// Closes the window and drops the cached bags when leaving the field (login, character select, cash
// shop). Called from set_stage_hook (resolution.cpp).
void StorageBag_OnLeaveField();

// Routed from clientsocket.cpp; the handler skips the opcode itself. Runs on the main thread
// (CClientSocket::ProcessPacket is reached from CWvsApp::WindowProc / Run).
void StorageBag_HandlePacket(CInPacket* pPacket);
