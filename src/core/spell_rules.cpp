#include "core/spell_rules.h"

namespace scssc::core
{
	BindVerdict ClassifyForStaff(const SpellTraits& a_spell, bool a_copyKeepsSpellValues)
	{
		if (!a_spell.isPlainSpell) {
			return BindVerdict::kNotASpell;
		}
		if (!a_spell.known) {
			return BindVerdict::kNotKnown;
		}
		if (!a_spell.fireAndForget && !a_spell.concentration) {
			return BindVerdict::kCastingType;
		}
		if (!a_spell.hasEffects) {
			return BindVerdict::kNoEffects;
		}
		if (a_copyKeepsSpellValues) {
			return BindVerdict::kAllowed;
		}
		if (a_spell.range != 0.0f) {
			return BindVerdict::kRange;
		}
		if (a_spell.fixedCastDuration != 0.0f) {
			return BindVerdict::kFixedDuration;
		}
		if (a_spell.ignoreResistance) {
			return BindVerdict::kIgnoreResistance;
		}
		if (a_spell.ignoreLOS) {
			return BindVerdict::kIgnoreLOS;
		}
		if (a_spell.noAbsorb && !a_spell.selfDelivery) {  // a self spell is never absorbed (design 3)
			return BindVerdict::kNoAbsorb;
		}
		return BindVerdict::kAllowed;
	}

	const char* Describe(BindVerdict a_verdict)
	{
		switch (a_verdict) {
		case BindVerdict::kAllowed:
			return "allowed";
		case BindVerdict::kNotASpell:
			return "not a spell";
		case BindVerdict::kNotKnown:
			return "not known";
		case BindVerdict::kCastingType:
			return "casting type";
		case BindVerdict::kNoEffects:
			return "no effects";
		case BindVerdict::kRange:
			return "has a range (summon or target location)";
		case BindVerdict::kFixedDuration:
			return "fixed cast duration";
		case BindVerdict::kIgnoreResistance:
			return "ignores resistance";
		case BindVerdict::kIgnoreLOS:
			return "ignores line of sight";
		case BindVerdict::kNoAbsorb:
			return "cannot be absorbed";
		}
		return "unknown";
	}
}
