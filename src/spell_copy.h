#pragma once

#include "core/spell_rules.h"

// A spell turned into a staff enchantment. One copy per spell, made on first use and kept until the game
// exits (design 4), so pointers handed to the engine never dangle.

namespace scssc::spell_copy
{
	// The copy for a_spell, made on first call. Null when the form factory fails.
	RE::EnchantmentItem* Get(RE::SpellItem* a_spell);

	// The spell a copy was made from; null for anything that is not one of our copies.
	RE::SpellItem* OriginalOf(const RE::MagicItem* a_item);

	bool IsCopy(const RE::MagicItem* a_item);

	// How many lookups reached the copy table's lock (runtime forms only) since the last call. T-PERF.
	std::uint64_t TakeLockedLookups();

	// kPostLoadGame: each cached copy must still be the form its FormID finds (a load may hand out runtime FormIDs
	// again). One that is not is left out of the cache (never deleted) and made anew on next use. T-COPY-ID.
	void VerifyAfterLoad();

	// The original spell's effect behind a copy's effect (same position among the copied effects); null otherwise.
	RE::Effect* OriginalEffectOf(const RE::MagicItem* a_copy, const RE::Effect* a_copyEffect);

	core::SpellTraits TraitsOf(const RE::SpellItem* a_spell, RE::PlayerCharacter* a_player);

	// The tip art the engine shows for a_item: the costliest effect's casting art (the engine's main effect).
	RE::BGSArtObject* TipArtOf(const RE::MagicItem* a_item);
}
