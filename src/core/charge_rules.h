#pragma once

// Staff-charge decisions for charge spells, independent of the game.
// Fire-and-forget costs one full payment per shot; concentration costs per second.

namespace scssc::core
{
	// Longest frame we bill; a longer gap (hitch, save stall) is skipped, not billed late (SCS uses the same 0.25 s).
	inline constexpr float kMaxBilledFrameSeconds = 0.25f;

	// Fire-and-forget: allowed only when the staff holds at least one full shot.
	[[nodiscard]] bool AllowFireAndForget(float a_charge, float a_cost);

	// Charge to remove after the engine call returned. A failed target-location cast
	// (rune/summon placement rejected) costs nothing.
	[[nodiscard]] float FireAndForgetDebit(float a_cost, bool a_targetLocationFailed);

	// Concentration may start with any charge left; it is billed per second while it runs.
	[[nodiscard]] bool AllowConcentrationStart(float a_charge, float a_costPerSecond);

	struct ChannelTick
	{
		float debit{ 0.0f };
		bool  interrupt{ false };
	};

	// One frame of a running concentration cast.
	[[nodiscard]] ChannelTick ConcentrationTick(float a_charge, float a_costPerSecond, float a_deltaSeconds);
}
