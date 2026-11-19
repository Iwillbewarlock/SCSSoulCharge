#include "core/spell_rules.h"

namespace scssc::core
{
	BindVerdict ClassifyForStaff(const SpellTraits& a_spell, bool a_copyKeepsSpellValues)
	{
		if (!a_spell.isPlainSpell) {
			return BindVerdict::kNotASpell;
		}
		if (a_spell.runtimeForm) {
			return BindVerdict::kRuntimeForm;
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
		case BindVerdict::kRuntimeForm:
			return "made during play";
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

	HeldBinding DecideHeld(bool a_bound, std::uint32_t a_spellId, bool a_formFound, bool a_runtimeForm, bool a_known)
	{
		if (!a_bound) {
			return HeldBinding::kNone;
		}
		// ResolveFormID passes 0xFF ids through unchanged, and a record removed from a plugin that is still loaded
		// resolves too: both leave an id that finds nothing, or a runtime form that is not the one bound (Carbon 2026-11-19).
		if (a_spellId == 0 || !a_formFound || a_runtimeForm || (a_spellId >> 24) == 0xFF) {
			return HeldBinding::kUnbind;
		}
		return a_known ? HeldBinding::kActive : HeldBinding::kDormant;
	}

	bool CastReleased(const ReleaseSignals& a_signals)
	{
		if (a_signals.releaseState || a_signals.castEvent || a_signals.effectApplied) {
			return true;
		}
		// The magicka drop alone is the weakest sign: a left-hand cast or a hit can drain as much (Astra, Opus, Carbon 2026-11-19).
		return a_signals.paidEnough && !a_signals.otherHandCast && !a_signals.slotChanged;
	}
}
