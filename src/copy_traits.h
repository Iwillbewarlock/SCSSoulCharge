#pragma once

// A staff copy is an EnchantmentItem, which does not override the MagicItem questions below (MagicItem.h:87-93,
// all answer 0/false): range, fixed cast duration, ignore resistance, ignore line of sight, no absorb, no dual-cast
// modifications, and skill usage (which school a cast trains, MagicItem.h:94). Hooked on the EnchantmentItem vtable only, so a copy answers with its original spell's values and
// every other enchantment keeps the engine's answer (user request 2026-11-17: keep the range of summon and
// target-location spells).

namespace scssc::copy_traits
{
	bool Install();

	// How many times the engine has asked any copy of a ranged spell for its range. cast_watch compares it across one
	// cast (T-RANGE): no question during a ranged cast means the engine read the range somewhere else.
	std::uint32_t RangeAsks();
}
