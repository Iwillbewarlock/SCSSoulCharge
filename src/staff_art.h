#pragma once

// The staff tip of a converted staff shows the casting art of the spell bound to it (user request
// 2026-11-07). The hand casts SCS's shared dummy enchantment, so its effect's casting art and light are
// swapped for those of the bound spell's main effect while a converted staff is held, and restored when
// none is. Hypothesis (to be judged by eye): the tip glow comes from that casting art.
// The binding is learned from SCS's menu equip call (we chain onto the call site SCS hooks) and from the
// spell SCS actually fires.

namespace scssc::staff_art
{
	void Install();  // kDataLoaded, after every plugin installed its load-time hooks
	void OnSpellFired(RE::PlayerCharacter* a_player, const RE::SpellItem* a_spell);
	void OnFrame(RE::PlayerCharacter* a_player);
}
