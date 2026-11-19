#pragma once

#include <cstdint>

// Which spells can be put on a staff. An enchantment copy loses the values EnchantmentItem does not
// override (SpellItem.h:72-79): range, fixed cast duration, ignore-resistance, ignore-LOS, no-absorb.

namespace scssc::core
{
	struct SpellTraits
	{
		bool  isPlainSpell{ false };  // SpellType::kSpell (not a power, shout, ability, ...)
		bool  known{ false };         // the player has the spell
		bool  fireAndForget{ false };
		bool  concentration{ false };
		bool  selfDelivery{ false };
		float range{ 0.0f };
		float fixedCastDuration{ 0.0f };
		bool  ignoreResistance{ false };
		bool  ignoreLOS{ false };
		bool  noAbsorb{ false };
		bool  hasEffects{ false };
		bool  runtimeForm{ false };   // made during play (0xFF FormID): after a load that id may name another form
	};

	enum class BindVerdict
	{
		kAllowed,
		kNotASpell,       // powers, shouts, abilities, scrolls ...
		kRuntimeForm,     // a spell another mod made during play (Carbon 2026-11-19)
		kNotKnown,
		kCastingType,     // constant effect or scroll-like casting
		kNoEffects,
		kRange,           // summons and target-location spells
		kFixedDuration,
		kIgnoreResistance,
		kIgnoreLOS,
		kNoAbsorb,        // only matters for spells that can hit others
	};

	// a_copyKeepsSpellValues: the copy answers range, fixed duration and the flags with the spell's own values
	// (copy_traits hook), so those no longer rule a spell out.
	[[nodiscard]] BindVerdict ClassifyForStaff(const SpellTraits& a_spell, bool a_copyKeepsSpellValues);

	[[nodiscard]] const char* Describe(BindVerdict a_verdict);

	// What the held staff's binding does this frame.
	enum class HeldBinding
	{
		kNone,     // no binding on this item
		kUnbind,   // the spell is gone for good: its plugin, its record, or a runtime spell saved before the load
		kDormant,  // the spell exists but is not known now (Heart of Magic swaps it out): the binding waits (user 2026-11-17)
		kActive,
	};
	[[nodiscard]] HeldBinding DecideHeld(bool a_bound, std::uint32_t a_spellId, bool a_formFound, bool a_runtimeForm, bool a_known);

	// What showed that a staff copy was really cast. A cast counts as released once any of these holds.
	struct ReleaseSignals
	{
		bool releaseState{ false };   // the right caster went through a release state (kUnk04 / kCasting)
		bool castEvent{ false };      // the game sent its cast event for the copy (never under DMMF)
		bool effectApplied{ false };  // one of the copy's effects reached a target (a summon: the caster)
		bool paidEnough{ false };     // at the end of the cast: magicka dropped by at least half the cost (cost >= 1)
		bool otherHandCast{ false };  // the left hand cast during the sample: the drop may be its
		bool slotChanged{ false };    // the hand slot changed during the sample: the drop may be another item's
	};
	[[nodiscard]] bool CastReleased(const ReleaseSignals& a_signals);
}
