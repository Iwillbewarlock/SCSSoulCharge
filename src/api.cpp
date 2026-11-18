#include "spell_copy.h"

// For other SKSE plugins (Heart of Magic first, user decision 2026-11-17): a staff casts a runtime copy of the bound
// spell (an EnchantmentItem), so a plugin that counts spell casts or weakens spells by FormID can ask which spell a
// copy stands for. Find with GetModuleHandleW(L"SCSSoulCharge.dll") and GetProcAddress. Safe from any thread.
// Version 1: ApiVersion, GetOriginalSpell. Version 2 adds GetHeldStaffSpell and GetNameMarker (2026-11-18). None lets
// an exception cross the C boundary.

#include "staff_mark.h"

extern "C" __declspec(dllexport) std::uint32_t SCSSoulCharge_ApiVersion() noexcept
{
	return 2;
}

// The spell bound to the staff the player holds now (its name carries the marker), or 0. A menu that draws its own icon
// shows the icon for this spell and strips the marker from the name.
extern "C" __declspec(dllexport) std::uint32_t SCSSoulCharge_GetHeldStaffSpell() noexcept
{
	return scssc::staff_mark::MarkedSpell();
}

// The marker text appended to that spell's name (UTF-8, starts with a space; empty when turned off). Valid for the whole
// game session. A mod that stores spell names (Heart of Magic's learning rename) strips it first.
extern "C" __declspec(dllexport) const char* SCSSoulCharge_GetNameMarker() noexcept
{
	static const std::string marker{ scssc::staff_mark::Marker() };
	return marker.c_str();
}

// The FormID of the spell a staff copy was made from, or 0 when a_item is not one of this mod's copies.
extern "C" __declspec(dllexport) std::uint32_t SCSSoulCharge_GetOriginalSpell(std::uint32_t a_item) noexcept
{
	try {
		const auto* item = RE::TESForm::LookupByID<RE::MagicItem>(a_item);
		const auto* original = scssc::spell_copy::OriginalOf(item);
		return original ? original->GetFormID() : 0;
	} catch (const std::exception& e) {
		logger::error("API GetOriginalSpell({:08X}) failed: {}", a_item, e.what());
		return 0;
	}
}
