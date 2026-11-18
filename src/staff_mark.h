#pragma once

#include <string_view>

// While the right hand holds a staff with an active binding, the bound spell's name ends with a marker (default
// " ★", [Display] sStaffMarker). Every magic menu lists spells by name, so the marker shows in the left list of the
// vanilla menu, SkyUI and Heart of Magic's menu alike (user decision 2026-11-18: the name everywhere, the picture icon
// in Heart of Magic's menu, which asks SCSSoulCharge_GetHeldStaffSpell and strips the marker itself).
// The name lives on the spell form and is not saved, so a crash leaves nothing behind.

namespace scssc::staff_mark
{
	// Main thread, every unpaused frame, after the hand slot is synced. a_spell: the bound spell of the held staff, or
	// null (no staff, no binding, or the spell is not known right now).
	void OnFrame(RE::SpellItem* a_spell);

	// Load, new game: take the marker off (the forms keep their names across loads within one game run).
	void Reset();

	std::string_view Marker();

	// a_name without a trailing marker.
	std::string_view Strip(std::string_view a_name);

	// The spell that carries the marker now (0 = none). Any thread.
	RE::FormID MarkedSpell();
}
