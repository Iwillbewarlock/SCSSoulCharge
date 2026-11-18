#pragma once

// The staff tip art (design 9). After the hand slot changes, the engine keeps the art it already attached; the
// player's right-hand caster is told to drop it and take the slot item's art: DetachCastingArt 34152 (true), then
// SetCastingArt 34150 (the HoM Black Flame route). Only while the staff is drawn, the caster idle and the player's
// 3D present; otherwise it waits for the first frame where that holds (draw, end of a cast). Proven 9/9 on
// 2026-11-17; preloading alone changed nothing (0/9).

namespace scssc::staff_art
{
	// Finds 34150/34152. Only on 1.6.1170; false elsewhere.
	bool Install();

	// Forget what is shown (load, new game): the next managed slot item gets a refresh.
	void Reset();

	// Main thread, every unpaused frame, after the hand slot is synced. a_staffHeld: a target staff is in the right hand.
	void OnFrame(RE::PlayerCharacter* a_player, float a_delta, bool a_staffHeld);
}
