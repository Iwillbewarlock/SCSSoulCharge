#pragma once

// Charge spells are paid from the staff's soul gem charge.
//  - ActorMagicCaster::CastSpellImmediate (vtable slot 0x01): SCS fires every staff spell through it
//    on the player's kOther caster (Casting.cpp:490, :782). For a charge spell we allow or refuse the
//    shot and bill the staff after it went out.
//  - A per-frame callback on the same Main::Update call site SCS uses (FrameHook.cpp:39-63) bills
//    running concentration casts, keeps converted staves' names, and stops empty channels.
// Charge lives in the hand's ItemCharge actor value while the staff is held (StaffCasting-spike.log:17-37).

namespace scssc::casting
{
	bool InstallCastHook();
	bool InstallFrameHook();

	// Charge this plugin took from a hand since the game started (refunds subtracted). Self-tests use it
	// to tell our billing apart from anything the engine drains.
	[[nodiscard]] float TotalBilled(bool a_left);
}
