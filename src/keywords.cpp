#include "PCH.h"

#include "keywords.h"

#include "core/staff_filter.h"
#include "pool.h"
#include "settings.h"

#include <unordered_map>
#include <unordered_set>

namespace scssc::keywords
{
	namespace
	{
		constexpr auto          kScsPlugin = "SpellCastingStaff.esp"sv;
		constexpr auto          kOwnPlugin = "SCSSoulCharge.esp"sv;
		constexpr RE::FormID    kScsStaffKeyword = 0x803;
		constexpr std::array<RE::FormID, 6> kBasicTierKeywords{ 0x8EE, 0x8F6, 0x8FF, 0x900, 0x901, 0x902 };  // SCS_Basic, Basic01..05
		constexpr std::size_t   kInitItemSlot = 0x13;

		using InitItemFn = void (*)(RE::TESObjectWEAP*);
		REL::Relocation<InitItemFn> g_originalInitItem;

		struct State
		{
			bool                           resolved{ false };
			std::uint32_t                  failedResolves{ 0 };
			RE::BGSKeyword*                staffKeyword{ nullptr };
			RE::BGSKeyword*                tierKeyword{ nullptr };
			core::StaffFilterSettings      filter;
			std::unordered_set<RE::FormID> converted;
			std::unordered_map<RE::FormID, bool> scriptAtInit;
			Stats                          stats;
		};

		State& GetState()
		{
			static State state;
			return state;
		}

		// First call happens during data load, when every plugin's forms already exist.
		void ResolveOnce(State& a_state)
		{
			if (a_state.resolved) {
				return;
			}
			auto* data = RE::TESDataHandler::GetSingleton();
			if (!data || !data->LookupModByName(kScsPlugin) || !data->LookupModByName(kOwnPlugin)) {
				a_state.resolved = true;  // a missing plugin will not appear later
				logger::critical("{} or {} is not active; no staff gets SCS keywords", kScsPlugin, kOwnPlugin);
				return;
			}
			const auto& settings = Settings::Get();
			a_state.staffKeyword = data->LookupForm<RE::BGSKeyword>(kScsStaffKeyword, kScsPlugin);
			a_state.tierKeyword = data->LookupForm<RE::BGSKeyword>(kBasicTierKeywords[static_cast<std::size_t>(settings.basicTier)], kScsPlugin);
			// Everything conversion needs must exist before the first keyword: a keyworded staff that is
			// never converted is blocked by SCS (Charge.cpp:128-142).
			if (!a_state.staffKeyword || !a_state.tierKeyword || !pool::Resolve()) {
				++a_state.failedResolves;
				logger::error("SCS keywords, charge spells or the shared ENCH not found (attempt {}); staff skipped",
					a_state.failedResolves);
				return;  // retried on the next staff, in case forms are initialised plugin by plugin
			}
			a_state.resolved = true;
			a_state.filter = ResolveFilterSettings(settings);
			a_state.stats.ready = true;
			logger::info("Keyword setup ready after {} failed attempt(s)", a_state.failedResolves);
		}

		bool HasScsKeyword(const RE::TESObjectWEAP* a_weapon, const State& a_state)
		{
			return a_weapon->HasKeyword(a_state.staffKeyword);
		}

		float RawEnchantmentCost(const RE::EnchantmentItem* a_enchantment)
		{
			// Manual-cost records carry their cost; auto-calc ones are priced by the engine later,
			// so here they only need at least one effect.
			if (a_enchantment->data.flags.all(RE::EnchantmentItem::EnchantmentFlag::kCostOverride)) {
				return static_cast<float>(a_enchantment->data.costOverride);
			}
			return a_enchantment->effects.empty() ? 0.0f : 1.0f;
		}

		void Process(RE::TESObjectWEAP* a_weapon)
		{
			auto& state = GetState();
			if (!a_weapon || !a_weapon->IsStaff()) {
				return;
			}
			ResolveOnce(state);
			++state.stats.staves;
			if (!state.stats.ready) {
				++state.stats.skipped;
				state.stats.skippedBeforeReady.push_back(a_weapon->GetFormID());
				return;
			}
			const auto* enchantment = a_weapon->formEnchanting;
			core::StaffFacts facts;
			facts.formID = a_weapon->GetFormID();
			facts.isStaffAnimation = true;
			facts.hasEnchantment = enchantment != nullptr;
			facts.enchantmentCost = enchantment ? RawEnchantmentCost(enchantment) : 0.0f;
			facts.hasScsKeyword = HasScsKeyword(a_weapon, state);
			facts.hasScript = a_weapon->HasVMAD();
			if (facts.hasScript) {
				++state.stats.scriptedSeen;
			}
			state.scriptAtInit[facts.formID] = facts.hasScript;
			const auto verdict = core::ClassifyStaff(facts, state.filter);
			if (verdict != core::StaffVerdict::kEligible) {
				++state.stats.skipped;
				if (verdict != core::StaffVerdict::kNotListed) {
					logger::info("STAFF skip {:08X} '{}': {}", facts.formID, a_weapon->GetName(), core::VerdictName(verdict));
				}
				return;
			}
			a_weapon->AddKeyword(state.staffKeyword);
			a_weapon->AddKeyword(state.tierKeyword);
			state.converted.insert(facts.formID);
			++state.stats.converted;
			state.stats.lastAddMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
			logger::info("STAFF add {:08X} '{}' ench={:08X} hasScript={}", facts.formID, a_weapon->GetName(), enchantment->GetFormID(), facts.hasScript);
		}

		void InitItemThunk(RE::TESObjectWEAP* a_weapon)
		{
			g_originalInitItem(a_weapon);
			Process(a_weapon);
		}
	}

	bool Install()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_TESObjectWEAP[0] };
		g_originalInitItem = vtable.write_vfunc(kInitItemSlot, InitItemThunk);
		logger::info("Hook installed: TESObjectWEAP::InitItemImpl (vtable slot 0x{:X})", kInitItemSlot);
		return true;
	}

	const Stats& GetStats()
	{
		return GetState().stats;
	}

	bool IsConverted(RE::FormID a_weapon)
	{
		return GetState().converted.contains(a_weapon);
	}

	std::optional<bool> ScriptFlagAtInit(RE::FormID a_weapon)
	{
		const auto& map = GetState().scriptAtInit;
		const auto  it = map.find(a_weapon);
		return it == map.end() ? std::nullopt : std::optional<bool>(it->second);
	}
}
