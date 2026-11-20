#pragma once

#include "hand.h"

// The staff tip art (design 9). After a hand slot changes, the engine keeps the art it already attached; that hand's
// caster is told to drop it and take the slot item's art: DetachCastingArt 34152 (true), then SetCastingArt 34150
// (the HoM Black Flame route). Only while the staff is drawn, the caster idle and the player's 3D present; otherwise
// it waits for the first frame where that holds (draw, end of a cast). Proven 9/9 on 2026-11-17 for the right hand;
// preloading alone changed nothing (0/9). The left hand uses its own caster the same way (2026-11-20, unproven).

namespace scssc::staff_art
{
	// Finds 34150/34152. Only on 1.6.1170; false elsewhere.
	bool Install();

	// Forget what is shown (load, new game): the next managed slot item gets a refresh.
	void Reset();

	// Main thread, every unpaused frame, once per hand, after the hand slots are synced. a_staffHeld: a target staff
	// is in that hand. Each hand's caster has its own art and its own state here.
	void OnFrame(RE::PlayerCharacter* a_player, float a_delta, Hand a_hand, bool a_staffHeld);
}
