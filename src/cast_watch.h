#pragma once

// One cast of a staff copy, watched from start to end:
//  - school experience comes from the engine: copy_traits answers GetSkillUsageData with the spell's data (the copy
//    alone trained nothing, 2026-11-17). T-XP judges it one second after the cast.
//  - self-tests (only with [SelfTest] bEnabled): T-PAY (magicka paid, staff charge untouched), T-COST (copy cost vs
//    the same spell by hand), T-RANGE (the engine asked the copy for its range during the cast), and a T-XP log line
//    for hand casts too, so the two can be compared.

namespace scssc::cast_watch
{
	void Reset();

	// kDataLoaded: the player's spell-cast events mark a copy's cast as released. Summons never showed the release
	// states the frame watch looked for (2026-11-17: magicka paid, "not released").
	void InstallEvents();

	// Main thread, every unpaused frame.
	void OnFrame(RE::PlayerCharacter* a_player, float a_delta);
}
