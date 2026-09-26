#pragma once

// Custom magic damage formula
// maxMAD = (INT × 5 + totalMAD) × totalMAD / 100 + INT
// See magicdmg.cpp for full documentation and address notes
void AttachMagicDamageMod();

// Stat-window RANGE row. Magicians use the same expression combat runs;
// other jobs use the physical 100%-skill estimate (weapon multiplier × stats × PAD).
// Both are computed in double and returned as 64-bit so high-stat characters do
// not wrap through int32 (INT_MIN / 1). Returns false when the context cannot
// be read — callers must then keep the client's own string.
bool MagicDmg_GetRange(long long* pnMin, long long* pnMax);
