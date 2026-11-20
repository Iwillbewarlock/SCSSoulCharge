#pragma once

#include "core/spell_rules.h"

#include <format>
#include <string>

// Every line the player can read on screen. English is built in; Data/SKSE/Plugins/SCSSoulCharge_Messages.ini
// ([Messages], saved as UTF-16 LE) replaces any of them, for a translation (final review 2026-11-19: the notices
// were fixed English). {0} and {1} are the names the line is given; a line that does not format is replaced by the
// English one when the file is read, so no notice can fail in game.

namespace scssc::messages
{
	enum class Msg
	{
		kPickDropped,
		kSingleOnly,      // {0} items share the list
		kNameLocked,
		kNowCasts,        // {0} staff, {1} spell
		kOwnAgain,        // {0} staff
		kCannotBind,      // {0} spell, {1} reason
		kReasonNotASpell,
		kReasonRuntime,
		kReasonNotKnown,
		kReasonCastingType,
		kReasonNoEffects,
		kDormant,         // {0} staff
		kSpellGone,       // {0} staff
		kTableFull,       // {0} bindings
		kOffRuntime,      // {0} game version
		kOffHook,
		kMenuHookFailed,
		kUninstallDone,   // {0} names restored
		kCount
	};

	// Reads the override file once (plugin load). Logs each line taken and each one refused.
	void Load();

	const std::string& Raw(Msg a_msg);
	const std::string& English(Msg a_msg);

	// The reason line for a refused spell.
	Msg ReasonFor(core::BindVerdict a_verdict);

	// The line with its names filled in. Names are always text (numbers too), so a translation cannot ask for a type
	// the game does not pass; pass exactly as many as the line takes (the comments on Msg).
	std::string Text(Msg a_msg, const std::string& a_first = {}, const std::string& a_second = {});
}
