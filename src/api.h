#pragma once

#include <cstdint>

// For other SKSE plugins (Heart of Magic first, user decision 2026-11-17). Two ways in.
//
// 1. Exported C functions. Find them with GetModuleHandleW(L"SCSSoulCharge.dll") and GetProcAddress, at or after
//    SKSE's kPostLoad (the settings they read are loaded in this plugin's SKSEPlugin_Load). Any thread. No exception
//    leaves them.
//      std::uint32_t SCSSoulCharge_ApiVersion()            3 (history below)
//      bool          SCSSoulCharge_IsActive()              false when nothing is bound: unsupported runtime, a hook
//                                                          failed, the magic-menu hook failed, or the uninstall option
//                                                          runs. GetNameMarker then answers "". It can turn false
//                                                          at the first game load: ask each time, do not store it
//      std::uint32_t SCSSoulCharge_GetOriginalSpell(id)    the spell a staff copy (0xFF EnchantmentItem) was made
//                                                          from, or 0 when id is not one of this mod's copies
//      std::uint32_t SCSSoulCharge_GetHeldStaffSpell()     the spell bound to the staff in the player's right hand,
//                                                          or 0 (no staff, no binding, spell not known now)
//      const char*   SCSSoulCharge_GetNameMarker()         the text appended to that spell's name, UTF-8, starts with
//                                                          a space; "" when turned off or the mod is off. Valid for
//                                                          the whole game session. A mod that stores spell names
//                                                          strips it only from the spell GetHeldStaffSpell returns
//                                                          (another spell may end with the same text on its own)
//    History: 1 ApiVersion, GetOriginalSpell. 2 (2026-11-18) GetHeldStaffSpell, GetNameMarker, and the SCST message.
//    3 (2026-11-19) IsActive; GetHeldStaffSpell answers with the marker turned off; GetNameMarker reads the INI value.
//
// 2. An SKSE message broadcast from sender "SCSSoulCharge" each time the player releases a staff cast of a bound
//    spell (from API 2 on), because the game sends no TESSpellCastEvent for staff (enchantment) casts. Listen with
//    SKSE::GetMessagingInterface()->RegisterListener("SCSSoulCharge", callback) at or after kPostLoad.
//    Type kStaffCastMessageType, data a StaffCastMessage. Sent on the main thread from the frame hook, once per cast
//    (a concentration cast once, when it starts streaming). The data lives only during the callback: copy the
//    fields, do not keep the pointer. Check dataLen >= 12; later versions only add fields at the end.

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
