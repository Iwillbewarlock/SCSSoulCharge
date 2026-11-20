#pragma once

// Effects a staff copy lands are re-pointed at the original spell's matching effect as they are applied
// (MagicTarget::AddTarget). The save keeps an active effect as a reference to its source form; a runtime copy is
// not in the save, so without this the copy's lasting effects vanished after a restart (T-SAVE, 7 of 7). Proven in
// game 2026-11-17 (T-SAVE-COLD 7/7). Concentration effects are left alone: they end with the cast.

#include "hand.h"

#include <cstdint>

namespace scssc::effect_redirect
{
	void Install();

	// How many effects of staff copies cast from that hand reached the player so far (a summon's, a self spell's). A
	// rise during a cast shows the copy was really cast, also for a summon that skips the release states and pays no
	// magicka. Any thread.
	std::uint32_t CopyApplications(Hand a_hand);

	// The same for effects whose casting source names no hand (other, instant). Any thread.
	std::uint32_t CopyApplicationsWithoutHand();
}
