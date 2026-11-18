#pragma once

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
	};

	enum class BindVerdict
	{
		kAllowed,
		kNotASpell,       // powers, shouts, abilities, scrolls ...
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
}
