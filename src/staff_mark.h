#pragma once

#include "hand.h"

#include <string_view>

// While a hand holds a staff with an active binding, the bound spell's name ends with that hand's marker (default
// " ★R" for the right hand, " ★L" for the left, [Display] sStaffMarkerRight / sStaffMarkerLeft, user decision
// 2026-11-20); a spell bound to the staves in both hands gets both (" ★R ★L"). Every magic menu lists spells by name,
// so the marker shows in the vanilla menu, SkyUI and Heart of Magic's menu alike. The name lives on the spell form and
// is not saved, so a crash leaves nothing behind. Only a marker this module added is taken off again.

namespace scssc::staff_mark
{
	// Main thread, every unpaused frame, after the hand slots are synced (and from the menu pick preview). The bound
	// spell of the staff in each hand, or null (no staff, no binding, or the spell is not known right now). The held
	// spells are tracked even with the marker turned off.
	void OnFrame(RE::SpellItem* a_right, RE::SpellItem* a_left);

	// Load, new game: take the markers off (the forms keep their names across loads within one game run).
	void Reset();

	// The marker text this module put on a_spell's name now (" ★R", " ★L", " ★R ★L"), or empty. Valid for the game
	// session (one of three strings set at plugin load). Any thread.
	const char* MarkerOn(RE::FormID a_spell);

	// a_spell's name without the marker this module put there. Main thread.
	std::string_view CleanName(const RE::SpellItem* a_spell);

	// The bound spell of the staff in that hand (0 = none), whether or not the marker is on. Any thread.
	RE::FormID HeldSpell(Hand a_hand);
}
