#pragma once

#include <cstdint>

// For other SKSE plugins. Two ways in (Heart of Magic first, user decision 2026-11-17):
//  1. Exported functions (api.cpp): SCSSoulCharge_ApiVersion() (2), SCSSoulCharge_GetOriginalSpell(formID),
//     SCSSoulCharge_GetHeldStaffSpell(), SCSSoulCharge_GetNameMarker().
//  2. An SKSE message broadcast from sender "SCSSoulCharge" each time the player releases a staff cast of a bound
//     spell, because the game sends no TESSpellCastEvent for staff (enchantment) casts. Listen with
//     SKSE::GetMessagingInterface()->RegisterListener("SCSSoulCharge", callback) after kPostLoad.
//     Message type kStaffCastMessageType, data a StaffCastMessage (dataLen = sizeof(StaffCastMessage)), sent on the
//     main thread from the frame hook, once per cast (a concentration cast once, when it starts streaming).

namespace scssc
{
	inline constexpr std::uint32_t kStaffCastMessageType = 'SCST';

	struct StaffCastMessage
	{
		std::uint32_t copy{ 0 };           // the runtime enchantment the staff cast (0xFF FormID)
		std::uint32_t spell{ 0 };          // the spell it was made from: count the cast as this spell
		std::uint32_t concentration{ 0 };  // 1 for a concentration spell
	};
	static_assert(sizeof(StaffCastMessage) == 12);
}
