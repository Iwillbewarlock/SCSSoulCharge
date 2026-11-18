#pragma once

// Effects a staff copy lands are re-pointed at the original spell's matching effect as they are applied
// (MagicTarget::AddTarget). The save keeps an active effect as a reference to its source form; a runtime copy is
// not in the save, so without this the copy's lasting effects vanished after a restart (T-SAVE, 7 of 7). Proven in
// game 2026-11-17 (T-SAVE-COLD 7/7). Concentration effects are left alone: they end with the cast.

namespace scssc::effect_redirect
{
	void Install();
}
