#include "plugin_state.h"
#include "settings.h"
#include "spell_copy.h"
#include "staff_mark.h"

// For other SKSE plugins; the contract is in api.h. None lets an exception cross the C boundary: the others only read
// atomics and strings set at plugin load, and GetOriginalSpell, which looks a form up, ends in a catch-all that
// returns 0, the one place a catch-all is right (an exception out of a noexcept function ends the game).

extern "C" __declspec(dllexport) std::uint32_t SCSSoulCharge_ApiVersion() noexcept
{
	return 4;
}

extern "C" __declspec(dllexport) bool SCSSoulCharge_IsActive() noexcept
{
	return scssc::IsActive();
}

extern "C" __declspec(dllexport) std::uint32_t SCSSoulCharge_GetHeldStaffSpell() noexcept
{
	return scssc::staff_mark::HeldSpell(scssc::Hand::kRight);
}

extern "C" __declspec(dllexport) std::uint32_t SCSSoulCharge_GetHeldStaffSpellInHand(std::uint32_t a_hand) noexcept
{
	return a_hand <= 1 ? scssc::staff_mark::HeldSpell(a_hand == 1 ? scssc::Hand::kLeft : scssc::Hand::kRight) : 0;
}

extern "C" __declspec(dllexport) const char* SCSSoulCharge_GetNameMarker() noexcept
{
	// Read every call, not frozen at the first one: a caller before our plugin load would have fixed the default
	// (Opus, Carbon 2026-11-19). The string is set once at plugin load and not changed after, so the pointer stays valid.
	if (!scssc::IsActive()) {
		return "";
	}
	// The suffix on the right-hand spell now (" ★R ★L" when both staves hold it), so an API 2-3 caller strips all of it
	// (Opus 2026-11-20). A left-only spell is invisible to the old pair of calls: GetNameMarkerOn is the fix.
	const char* on = scssc::staff_mark::MarkerOn(scssc::staff_mark::HeldSpell(scssc::Hand::kRight));
	return *on ? on : scssc::Settings::Get().staffMarkerRight.c_str();
}

extern "C" __declspec(dllexport) const char* SCSSoulCharge_GetNameMarkerOn(std::uint32_t a_spell) noexcept
{
	return scssc::staff_mark::MarkerOn(a_spell);
}

extern "C" __declspec(dllexport) std::uint32_t SCSSoulCharge_GetOriginalSpell(std::uint32_t a_item) noexcept
{
	try {
		const auto* item = RE::TESForm::LookupByID<RE::MagicItem>(a_item);
		const auto* original = scssc::spell_copy::OriginalOf(item);
		return original ? original->GetFormID() : 0;
	} catch (...) {  // see the top of the file
		return 0;
	}
}
