#pragma once

// ============================================================
// coloringprism.h: the entry points the Coloring Prism window (item 5782000) exposes to the
// rest of the DLL.
//
// The window Detours two addresses nobody else owns -- CWvsContext::SendEtcCashItemUseRequest
// (0x00A1DC5B) and CDraggableSkill::OnDropped (0x004FAA22). Everything else it needs is an
// address another feature of this DLL already Detours, and a second Detour there would break
// whichever installed first, so it is DISPATCHED INTO from those owners instead:
//
//   0x004863D5 get_consume_cash_item_type    damageskinpicker.cpp  -> ColorPrism_ConsumeCashItemType
//   0x00A0A63F SendConsumeCashItemUseRequest damageskinpicker.cpp  -> ColorPrism_OnConsumeCashUse
//   0x004EF140 CDraggableItem::OnDropped     storagebag.cpp        -> ColorPrism_HandleItemDrop
//   0x00A06E55 CWvsContext::TryCloseUI       cashshopwnd.cpp       -> ColorPrism_TryCloseUI
//
// coloringprism.cpp includes this itself, so the compiler checks these against the
// definitions rather than leaving the two to drift.
// ============================================================

// Startup: installs the two Detours above.
void AttachColoringPrismMod();

// Is this the Coloring Prism (5782000)?
bool ColorPrism_IsPrismItem(int nItemID);

// The answer get_consume_cash_item_type (0x004863D5) must give for nItemID, or 0 when the id is
// not the prism and the caller should fall through to the stock answer.
//
// The client's own table only covers groups 500..561, so group 578 answers 0 and a prism
// double-click is dropped before any use request exists. The mod answers 1 -- the same value
// damageskinpicker.cpp already returns for 5910000, which routes the double-click into
// SendConsumeCashItemUseRequest (0x00A0A63F) where it is intercepted. The gate has twelve
// call sites (tooltips, icons, drop/move legality), hence the exact-id test.
int ColorPrism_ConsumeCashItemType(int nItemID);

// Offer a cash-item use to the prism, from a SendConsumeCashItemUseRequest /
// SendEtcCashItemUseRequest hook. Returns true if it was the prism and the window was opened,
// in which case the caller must SWALLOW the use: the prism is consumed by the server when a dye
// is confirmed, never by the stock use path.
bool ColorPrism_OnConsumeCashUse(int nPOS, int nItemID);

// Open the window for a prism at inventory position nPOS (no id check; prefer the above).
void ColorPrism_OnUse(int nPOS, int nItemID);

// Offer a drag-and-drop to the window, from the CDraggableItem::OnDropped hook.
//
// `pTo` is the window the drop was addressed to, as the engine resolved it; the window accepts
// either its own pointer or its pointer plus four (CWnd inherits IUIMsgHandler at +4). A NULL
// pTo is allowed: the window then tests whether the cursor is over its drop well.
//
// `invType` / `invPos` are the SOURCE of the drag, read off the draggable at +0x18 / +0x1C. The
// caller must NOT offer drags whose source is a custom window that puts its own meaning in
// those fields (the storage bag writes 2 / bagSlot there).
//
// Returns true if the drop was consumed; the caller must then not also move the item.
bool ColorPrism_HandleItemDrop(void* pTo, int invType, int invPos);

// Offer a dragged SKILL to the window. Called from this file's own CDraggableSkill::OnDropped
// Detour (0x004FAA22, vtable 0x00B39810 slot 1); the skill id is at draggable+0x18.
bool ColorPrism_HandleSkillDrop(void* pTo, int skillId);

// Esc: the window sits on CWvsContext::m_apStackForTab while open. cashshopwnd.cpp's
// TryCloseUI hook asks this; true = pWnd was the prism window and it is now closed.
bool ColorPrism_TryCloseUI(void* pWnd);
