#pragma once

#include <string_view>

// While the right hand holds a staff with an active binding, the bound spell's name ends with a marker (default
// " ★", [Display] sStaffMarker). Every magic menu lists spells by name, so the marker shows in the left list of the
// vanilla menu, SkyUI and Heart of Magic's menu alike (user decision 2026-11-18: the name everywhere, the picture icon
// in Heart of Magic's menu, which asks SCSSoulCharge_GetHeldStaffSpell and strips the marker itself).
// The name lives on the spell form and is not saved, so a crash leaves nothing behind. Only a marker this module
// added is taken off again (Astra 2026-11-19).

namespace scssc::staff_mark
{
	// Main thread, every unpaused frame, after the hand slot is synced (and from the menu pick preview). a_spell: the
	// bound spell of the held staff, or null (no staff, no binding, or the spell is not known right now). The held
	// spell is tracked even with the marker turned off.
	void OnFrame(RE::SpellItem* a_spell);

	// Load, new game: take the marker off (the forms keep their names across loads within one game run).
	void Reset();

	std::string_view Marker();

	// a_spell's name without the marker when this module put it there. Main thread.
	std::string_view CleanName(const RE::SpellItem* a_spell);

	// The bound spell of the staff in the right hand (0 = none), whether or not the marker is on. Any thread.
	RE::FormID HeldSpell();
}
