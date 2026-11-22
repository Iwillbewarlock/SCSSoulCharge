#include "resource_hook.h"

#include "detour_helper.h"
#include "runtime_check.h"
#include "spell_copy.h"

#include <atomic>
#include <filesystem>
#include <intrin.h>

#include <Windows.h>
#include <mutex>
#include <set>

namespace scssc::resource_hook
{
	namespace
	{
		using ResourceFn = RE::ActorValue (*)(RE::MagicItem*, RE::MagicSystem::CastingSource);
		ResourceFn g_original = nullptr;

		using CostFn = float (*)(const RE::MagicItem*, RE::Actor*);
		CostFn g_originalCost = nullptr;

		// The game's own lookup inside 38823 (SE 37868; 38823+0x85 on the AE builds read, game +6C72E5 on 1.6.1170)
		// turns the item enchantment's resource into the HUD charge ratio; it keeps the engine's answer (2026-11-16
		// spike: answering magicka there broke the staff charge). Found at start by runtime_check, set before the hook.
		std::uintptr_t g_itemChargeReturn = 0;

		// Every distinct (caller, casting source) that asks for a copy's resource, once a game run: the left hand's HUD
		// charge path may call from another site than the right hand's (38823+0x85), known for the right hand only (Astra 2026-11-20). This
		// line is how the left caller is found in game; a new caller is not trusted until it is shown to be the HUD.
		std::mutex                                                          g_callersLock;
		std::set<std::pair<std::uintptr_t, RE::MagicSystem::CastingSource>> g_callers;

		// The file name and offset of the module that holds a_address: the 2026-11-20 test saw callers outside
		// SkyrimSE.exe (another plugin hooking 33817 after us, guess), and the name says which.
		std::string ModuleOf(std::uintptr_t a_address)
		{
			HMODULE module = nullptr;
			if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(a_address), &module) ||
				!module) {
				return "no module";
			}
			std::wstring path(MAX_PATH, L'\0');
			const auto   length = ::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
			path.resize(length);
			const auto name = std::filesystem::path(path).filename().wstring();
			return std::format("{}+{:X}", SKSE::stl::utf16_to_utf8(name).value_or("?"), a_address - reinterpret_cast<std::uintptr_t>(module));
		}

		void LogCallerOnce(std::uintptr_t a_return, std::uintptr_t a_caller, RE::MagicSystem::CastingSource a_source, RE::ActorValue a_engine, RE::ActorValue a_answer)
		{
			{
				const std::scoped_lock lock{ g_callersLock };
				if (!g_callers.emplace(a_caller, a_source).second) {
					return;
				}
			}
			// The raw address too, so two callers the module lookup cannot name still tell apart (Astra 2026-11-20).
			logger::info("copy resource asked from {} (address {:X}, game +{:X}) for casting source {} (0 left, 1 right): engine {}, answered {}",
				ModuleOf(a_return), a_return, a_caller, static_cast<int>(a_source), static_cast<int>(a_engine), static_cast<int>(a_answer));
		}

		RE::ActorValue Thunk(RE::MagicItem* a_item, RE::MagicSystem::CastingSource a_source)
		{
			const auto engine = g_original(a_item, a_source);
			if (!spell_copy::IsCopy(a_item)) {
				return engine;
			}
			const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
			const auto caller = returnAddress - REL::Module::get().base();
			const auto answer = returnAddress == g_itemChargeReturn ? engine : RE::ActorValue::kMagicka;
			LogCallerOnce(returnAddress, caller, a_source, engine, answer);
			return answer;
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

		// ActorMagicCaster::CheckCast (vfunc 0x0A, ActorMagicCaster.h:50). The engine names the reason by the item's
		// kind: an enchantment that cannot be paid is "not enough charge", though a copy pays magicka. The user saw the
		// charge notice with low magicka (2026-11-20); the reason is turned into kMagicka so the game says magicka.
		// Whether the caller shows the notice from this reason (and not before) is checked in game (T-CANNOTCAST).
		struct CheckCastHook
		{
			static bool Thunk(RE::ActorMagicCaster* a_this, RE::MagicItem* a_spell, bool a_dualCast, float* a_strength, RE::MagicSystem::CannotCastReason* a_reason,
				bool a_useBaseValueForCost)
			{
				const bool ok = original(a_this, a_spell, a_dualCast, a_strength, a_reason, a_useBaseValueForCost);
				if (!ok && a_reason && *a_reason == RE::MagicSystem::CannotCastReason::kItemCharge && spell_copy::IsCopy(a_spell)) {
					*a_reason = RE::MagicSystem::CannotCastReason::kMagicka;
					static std::atomic<bool> logged{ false };
					if (!logged.exchange(true)) {
						logger::info("T-CANNOTCAST copy {:08X}: reason 'not enough charge' turned into 'not enough magicka' (once a game run)", a_spell->GetFormID());
					}
				}
				return ok;
			}
			static inline REL::Relocation<decltype(&Thunk)> original;
		};

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

	bool Prepare()
	{
		const auto lookup = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(33817, 34609) }.address();
		const auto cost = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(11213, 11321) }.address();
		// Callers keep calling the function's own address after a detour (Detours patches the function's entry), so
		// the scan is the same before or after; it runs here, with the other checks, before anything is written.
		g_itemChargeReturn = runtime_check::ItemChargeReturn(lookup);
		const bool lookupOk = runtime_check::DetourTargetOk(lookup, "resource lookup 33817/34609");
		const bool costOk = runtime_check::DetourTargetOk(cost, "CalculateCost 11213/11321");
		if (!g_itemChargeReturn) {
			logger::critical("HUD charge caller not found; the mod stays off (a copy would break the staff charge display)");
		}
		g_original = reinterpret_cast<ResourceFn>(lookup);
		g_originalCost = reinterpret_cast<CostFn>(cost);
		return g_itemChargeReturn && lookupOk && costOk;
	}

	bool Install()
	{
		if (!Attach(reinterpret_cast<void**>(&g_original), reinterpret_cast<void*>(&Thunk), "cost resource lookup")) {
			return false;
		}
		if (!Attach(reinterpret_cast<void**>(&g_originalCost), reinterpret_cast<void*>(&CostThunk), "magic item cost")) {
			// The first detour must not stay behind while the mod reports itself off (reviewer 2026-11-22).
			const auto err = detour::Detach(reinterpret_cast<void**>(&g_original), reinterpret_cast<void*>(&Thunk));
			logger::critical("cost resource lookup hook removed again (Detours error {})", err);
			return false;
		}
		REL::Relocation<std::uintptr_t> casterVtable{ RE::VTABLE_ActorMagicCaster[0] };
		CheckCastHook::original = casterVtable.write_vfunc(0x0A, CheckCastHook::Thunk);
		logger::info("Hook installed: ActorMagicCaster::CheckCast (copies report 'not enough magicka')");
		return true;
	}
}
