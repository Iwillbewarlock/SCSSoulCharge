#pragma once

// Which game builds this DLL runs on, and the start-up checks that make one DLL safe on builds nobody here can run
// (design 13, 2026-11-22). Each engine address the mod calls or hooks is looked up for the running build (SE 1.5.97
// id or AE id) and then checked in the running game's code: the functions it must call, the members it must touch,
// or the function a hooked CALL must reach. Any failure keeps the whole mod off with the in-game notice instead of
// calling a wrong function. Every check writes one "T-RUNTIME PASS|FAIL <id>" line to SCSSoulCharge.log.
//
// Where the ids come from: AE ids were read in 1.6.1170 code (in game) and in the 1.7.104 executable (offline); every
// AE address library 1.6.317-1.6.1179 and 1.7.104 has them, with the same function sizes. SE ids come from meh321's
// "Attempt at matching 1.5.97.0 to 1.6.318.0" (Address Library, Nexus file 243208), checked against five known
// SE/AE pairs; DetachCastingArt (34152) is not in that list and its SE id 33371 follows from the order of its
// neighbours (34149/34150/34153 -> 33368/33369/33372, equal sizes). The SE side is checked only by these start-up tests.

namespace scssc::runtime_check
{
	// 1.5.97, or AE 1.6.317 up to (not including) 1.8. Other 1.5.x builds and VR are not supported.
	[[nodiscard]] bool SupportedRuntime(const REL::Version& a_version);

	// Each returns the checked address, or 0 after logging why (the caller then keeps the mod off).
	[[nodiscard]] std::uintptr_t PreloadItem();       // SE 11195 / AE 11303: MagicItem preload (+0x80 count, +0x88 file)
	[[nodiscard]] std::uintptr_t ReleaseItem();       // SE 11198 / AE 11306
	[[nodiscard]] std::uintptr_t SetCastingArt();     // SE 33369 / AE 34150
	[[nodiscard]] std::uintptr_t DetachCastingArt();  // SE 33371 / AE 34152

	// The return address of the HUD charge lookup: the only CALL to a_resourceLookup (33817/34609, the original
	// function address, before any detour) inside SE 37868 / AE 38823. 0 when there is not exactly one.
	[[nodiscard]] std::uintptr_t ItemChargeReturn(std::uintptr_t a_resourceLookup);

	// The Main::Update CALL this mod hooks for its per-frame work, or 0. Candidates per build; a candidate counts
	// when it is a CALL to one of the two known per-frame functions, or to code outside SkyrimSE.exe (another plugin
	// already hooked that same call).
	[[nodiscard]] std::uintptr_t FrameCallSite();

	// The magic menu's spell-equip CALL at SE 37952 / AE 38908 +0xD7 (the site SCS hooks). Its destination must be
	// 38928 (AE) or, on SE, a function start in the unwind table (the SE match for 38928 is weak; the offset comes from
	// SCS 2.1.1, which supports 1.5.97); executable code outside SkyrimSE.exe (SCS or another plugin hooked it first)
	// is accepted on both.
	[[nodiscard]] bool MenuCallSiteOk(std::uintptr_t a_site);

	// The ten virtual-function slots this mod overwrites (EnchantmentItem 0x59-0x60, ActorMagicCaster 0x0A,
	// MagicTarget::AddTarget in Character and PlayerCharacter) each still hold the expected engine function, or code
	// outside SkyrimSE.exe (another plugin replaced it first). Every slot is checked and logged; false if any fails.
	[[nodiscard]] bool VtableSlotsOk();

	// A function whose entry Detours patches (33817/34609, 11213/11321: ids from CommonLib) must start a function in
	// the unwind table; false = it does not, after logging.
	[[nodiscard]] bool DetourTargetOk(std::uintptr_t a_address, std::string_view a_name);

	// a_address lies inside SkyrimSE.exe's image.
	[[nodiscard]] bool InGameModule(std::uintptr_t a_address);
}
