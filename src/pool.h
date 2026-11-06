#pragma once

#include "core/pool_table.h"

// The 1024 placeholder SPELs of SCSSoulCharge.esp and the enchantment each one stands for.

namespace scssc::pool
{
	inline constexpr std::uint32_t kPoolSize = 1024;

	// kDataLoaded: finds the placeholder spells and SCS's shared staff enchantment. False = feature off.
	bool Resolve();
	[[nodiscard]] bool IsReady();

	// The charge spell for a staff enchantment; assigns and fills a slot on first use.
	// nullptr when the pool is full or not ready.
	[[nodiscard]] RE::SpellItem* SpellFor(RE::EnchantmentItem* a_enchantment);

	// Existing charge spell for the enchantment, without assigning one.
	[[nodiscard]] RE::SpellItem* FindSpell(const RE::EnchantmentItem* a_enchantment);

	// Reverse lookup for the cast hook: which staff enchantment a charge spell came from.
	[[nodiscard]] RE::EnchantmentItem* EnchantmentOf(const RE::MagicItem* a_spell);
	[[nodiscard]] bool                 IsPoolSpell(const RE::MagicItem* a_spell);
	[[nodiscard]] bool                 PlayerKnowsAnyPoolSpell();

	// SpellCastingStaff.esp 0x80B, the shared dummy staff enchantment.
	[[nodiscard]] RE::EnchantmentItem* SharedEnchantment();

	// Co-save support. Clear empties every placeholder; Refill re-applies the table after a load.
	[[nodiscard]] core::PoolTable& Table();
	void                           Clear();
	void                           RefillFromTable();
}
