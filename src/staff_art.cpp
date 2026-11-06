#include "PCH.h"

#include "staff_art.h"

#include "keywords.h"
#include "pool.h"

#include <cstring>

namespace scssc::staff_art
{
	namespace
	{
		constexpr RE::FormID kRightHandSlot = 0x13F42;
		constexpr RE::FormID kLeftHandSlot = 0x13F43;

		using MenuEquipFn = void (*)(RE::ActorEquipManager*, RE::Actor*, RE::SpellItem*, RE::BGSEquipSlot**);
		MenuEquipFn g_previousMenuEquip = nullptr;

		struct Original
		{
			bool                captured{ false };
			RE::EffectSetting*  effect{ nullptr };  // SCS's dummy effect on the shared enchantment
			RE::BGSArtObject*   castingArt{ nullptr };
			RE::TESObjectLIGH*  light{ nullptr };
		};
		Original                g_original;
		const RE::EffectSetting* g_applied = nullptr;  // effect whose art is currently shown, nullptr = SCS's own

		bool CaptureOriginal()
		{
			if (g_original.captured) {
				return g_original.effect != nullptr;
			}
			g_original.captured = true;
			auto* shared = pool::SharedEnchantment();
			if (!shared || shared->effects.empty() || !shared->effects[0] || !shared->effects[0]->baseEffect) {
				logger::warn("Staff art: SCS shared enchantment has no effect; tip art stays as is");
				return false;
			}
			g_original.effect = shared->effects[0]->baseEffect;
			g_original.castingArt = g_original.effect->data.castingArt;
			g_original.light = g_original.effect->data.light;
			return true;
		}

		bool HoldsConvertedStaff(RE::PlayerCharacter* a_player, bool a_left)
		{
			auto* weapon = a_player->GetEquippedObject(a_left);
			return weapon && keywords::IsConverted(weapon->GetFormID());
		}

		void Restore()
		{
			if (g_original.effect && g_applied) {
				g_original.effect->data.castingArt = g_original.castingArt;
				g_original.effect->data.light = g_original.light;
				g_applied = nullptr;
				logger::info("STAFF ART restored to SCS's own");
			}
		}

		void Apply(const RE::SpellItem* a_spell)
		{
			if (!a_spell || !CaptureOriginal()) {
				return;
			}
			const auto* main = a_spell->GetCostliestEffectItem();
			const auto* source = main ? main->baseEffect : nullptr;
			if (!source || source == g_applied) {
				return;
			}
			g_original.effect->data.castingArt = source->data.castingArt;
			g_original.effect->data.light = source->data.light;
			g_applied = source;
			logger::info("STAFF ART <- '{}' {:08X}: castingArt={:08X} light={:08X}", a_spell->GetName(), a_spell->GetFormID(),
				source->data.castingArt ? source->data.castingArt->GetFormID() : 0u, source->data.light ? source->data.light->GetFormID() : 0u);
		}

		// Chained after SCS's own interception of the magic menu's spell equip (SCS MenuEquipHook.cpp).
		void MenuEquipThunk(RE::ActorEquipManager* a_manager, RE::Actor* a_actor, RE::SpellItem* a_spell, RE::BGSEquipSlot** a_slot)
		{
			g_previousMenuEquip(a_manager, a_actor, a_spell, a_slot);
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || a_actor != player || !a_spell || !a_slot || !*a_slot) {
				return;
			}
			const bool left = (*a_slot)->GetFormID() == kLeftHandSlot;
			if ((left || (*a_slot)->GetFormID() == kRightHandSlot) && HoldsConvertedStaff(player, left)) {
				Apply(a_spell);  // the staff is still in the hand: SCS consumed the equip as a binding
			}
		}
	}

	void Install()
	{
		// SCS installs its required menu hook at kDataLoaded and refuses to run if the call no longer goes
		// to the engine (in-game test 2026-11-07: "REQUIRED MENU HOOK FAILED" when we chained first).
		// So this runs on the first kPostLoadGame/kNewGame, after SCS, and only once.
		static bool installed = false;
		if (installed) {
			return;
		}
		installed = true;
		const REL::Relocation<std::uintptr_t> parent{ REL::RelocationID(37952, 38908) };
		const auto site = parent.address() + 0xD7;  // the call SCS hooks (SCS MenuEquipHook.cpp:42-43)
		if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8) {
			logger::warn("Staff art: menu equip call site is not a CALL; bindings are learned from fired spells only");
			return;
		}
		std::int32_t displacement = 0;
		std::memcpy(&displacement, reinterpret_cast<const void*>(site + 1), sizeof(displacement));
		g_previousMenuEquip = reinterpret_cast<MenuEquipFn>(site + 5 + static_cast<std::intptr_t>(displacement));
		auto& trampoline = SKSE::GetTrampoline();
		trampoline.write_call<5>(site, MenuEquipThunk);
		logger::info("Hook installed: magic menu spell equip call (chained after {:X})", reinterpret_cast<std::uintptr_t>(g_previousMenuEquip));
	}

	void OnSpellFired(RE::PlayerCharacter* a_player, const RE::SpellItem* a_spell)
	{
		if (a_player && (HoldsConvertedStaff(a_player, true) || HoldsConvertedStaff(a_player, false))) {
			Apply(a_spell);
		}
	}

	void OnFrame(RE::PlayerCharacter* a_player)
	{
		if (g_applied && !HoldsConvertedStaff(a_player, true) && !HoldsConvertedStaff(a_player, false)) {
			Restore();  // SCS's own staves keep their own look
		}
	}
}
