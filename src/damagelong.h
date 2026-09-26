#pragma once

// 64-bit damage lines (damagelong.cpp).
//
// anDamage[] and every int the client passes around stay 32-bit. A line whose real value does not
// fit below kDamageTagBase is stored as a TAG: an int in [kDamageTagBase, kDamageTagBase + ring)
// that indexes a ring of real 64-bit values. Everything that needs the real number (the attack
// packet encode, the Effect_HP digit string, Shadow Partner) resolves the tag; everything else
// just sees a number around 2.147e9, which is what the int already saturated to before.

void AttachDamageLongMod();

constexpr int kDamageTagBase = 0x7FFF0000;

// Real value of an anDamage int: the ring entry for a tag, the magnitude otherwise.
long long DamageLong_Resolve(int nDamage);

// True when nDamage (crit bit ignored) is a live tag.
bool DamageLong_IsTag(int nDamage);

// int to store for a real value: the value itself below kDamageTagBase, else a new tag.
int DamageLong_Store(long long nReal);
