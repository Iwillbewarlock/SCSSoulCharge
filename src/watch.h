#pragma once

// Self-test observers that need the game running (all write SELFTEST lines only when [SelfTest] bEnabled=1):
//  T-C2b  the hand of a converted staff casts SCS's shared ENCH, not the staff's own one
//  T-C5   the engine drains no charge for that native cast (only our billing changes the charge)
//  T-C4   the self-test staff (SCSSoulCharge.esp C00, vanilla Staff of Fireballs copy): the engine's
//         drain per shot compared with CalculateMagickaCost(player) and (nullptr)
//  T-C3   after unequipping a converted staff its ExtraCharge equals the charge it had in the hand
// Plus: warns when StaffCasting.dll is loaded (the two plugins conflict).

namespace scssc::watch
{
	void OnDataLoaded();
	void OnFrame(RE::PlayerCharacter* a_player);
	void OnUnequip(RE::PlayerCharacter* a_player, const RE::TESObjectWEAP* a_base);
}
