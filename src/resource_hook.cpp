#include "resource_hook.h"

#include "detour_helper.h"
#include "spell_copy.h"

#include <intrin.h>

namespace scssc::resource_hook
{
	namespace
	{
		using ResourceFn = RE::ActorValue (*)(RE::MagicItem*, RE::MagicSystem::CastingSource);
		ResourceFn g_original = nullptr;

		using CostFn = float (*)(const RE::MagicItem*, RE::Actor*);
		CostFn g_originalCost = nullptr;

		// The game's own lookup at +6C72E5 (1.6.1170, inside 38823) turns the item enchantment's resource into the HUD
		// charge ratio; it keeps the engine's answer (2026-11-16 spike: answering magicka there broke the staff charge).
		constexpr std::uintptr_t kItemChargeCaller1170 = 0x6C72E5;

		RE::ActorValue Thunk(RE::MagicItem* a_item, RE::MagicSystem::CastingSource a_source)
		{
			const auto engine = g_original(a_item, a_source);
			if (!spell_copy::IsCopy(a_item)) {
				return engine;
			}
			static const bool is1170 = REL::Module::get().version() == REL::Version{ 1, 6, 1170, 0 };
			const auto        caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - REL::Module::get().base();
			return is1170 && caller == kItemChargeCaller1170 ? engine : RE::ActorValue::kMagicka;
		}

		// MagicItem::CalculateCost (11213/11321, CommonLib MagicItem.cpp:42). A copy's own cost skipped the spell's perks
		// and used the Enchanting skill for the skill discount (2026-11-17: Frost Rune 120.00 from the staff, 105.80 by
		// hand, T-COST). A copy now costs exactly what its spell costs this caster.
		float CostThunk(const RE::MagicItem* a_item, RE::Actor* a_caster)
		{
			if (auto* spell = spell_copy::OriginalOf(a_item)) {
				return g_originalCost(spell, a_caster);
			}
			return g_originalCost(a_item, a_caster);
		}

		bool Attach(void** a_target, void* a_detour, const char* a_what)
		{
			const auto address = reinterpret_cast<std::uintptr_t>(*a_target);
			if (const auto err = detour::Attach(a_target, a_detour); err != 0) {
				*a_target = nullptr;
				logger::critical("{} hook not installed at {:X} (Detours error {}); the mod stays off", a_what, address, err);
				return false;
			}
			logger::info("Hook installed: {} at {:X}", a_what, address);
			return true;
		}
	}

	bool Install()
	{
		g_original = reinterpret_cast<ResourceFn>(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(33817, 34609) }.address());
		g_originalCost = reinterpret_cast<CostFn>(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(11213, 11321) }.address());
		return Attach(reinterpret_cast<void**>(&g_original), reinterpret_cast<void*>(&Thunk), "cost resource lookup") &&
		       Attach(reinterpret_cast<void**>(&g_originalCost), reinterpret_cast<void*>(&CostThunk), "magic item cost");
	}
}
