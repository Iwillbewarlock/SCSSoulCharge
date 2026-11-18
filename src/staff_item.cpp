#include "staff_item.h"

#include "settings.h"

namespace scssc::staff_item
{
	namespace
	{
		constexpr auto       kScsPlugin = "SpellCastingStaff.esp"sv;
		constexpr RE::FormID kScsStaffKeyword = 0x803;  // SpellCastingStaff_Keyword

		RE::BGSKeyword*                   g_scsKeyword = nullptr;
		std::unordered_set<RE::FormID>    g_excluded;
	}

	void OnDataLoaded()
	{
		auto* data = RE::TESDataHandler::GetSingleton();
		g_scsKeyword = data ? data->LookupForm<RE::BGSKeyword>(kScsStaffKeyword, kScsPlugin) : nullptr;
		logger::info("staff filter: SCS keyword {}", g_scsKeyword ? "found (SCS staves are left alone)" : "not found (SCS not installed)");
		g_excluded = ResolveExcludedStaves(Settings::Get());
		logger::info("staff filter: {} staves excluded by the INI", g_excluded.size());
	}

	bool IsTarget(const RE::TESObjectWEAP* a_weapon)
	{
		return a_weapon && a_weapon->IsStaff() && !(g_scsKeyword && a_weapon->HasKeyword(g_scsKeyword)) && !g_excluded.contains(a_weapon->GetFormID());
	}

	std::optional<Held> RightHand(RE::PlayerCharacter* a_player)
	{
		auto* weapon = a_player ? a_player->GetEquippedObject(false) : nullptr;
		auto* staff = weapon ? weapon->As<RE::TESObjectWEAP>() : nullptr;
		if (!IsTarget(staff)) {
			return std::nullopt;
		}
		auto* changes = a_player->GetInventoryChanges();
		if (!changes || !changes->entryList) {
			return std::nullopt;
		}
		for (auto* entry : *changes->entryList) {
			if (!entry || entry->object != staff || !entry->extraLists) {
				continue;
			}
			for (auto* list : *entry->extraLists) {
				// The right hand's item: worn (kWorn), not the left hand's copy of the same staff (kWornLeft).
				if (list && list->HasType(RE::ExtraDataType::kWorn)) {
					return Held{ staff, list, list->GetCount() };
				}
			}
		}
		return std::nullopt;
	}

	std::optional<core::StaffKey> KeyOf(RE::PlayerCharacter* a_player, const Held& a_held, bool a_assign)
	{
		if (!a_held.list) {
			return std::nullopt;
		}
		if (a_assign && !a_held.list->HasType(RE::ExtraDataType::kUniqueID)) {
			if (auto* changes = a_player->GetInventoryChanges()) {
				changes->SetUniqueID(a_held.list, nullptr, a_held.staff);
			}
		}
		const auto* id = a_held.list->GetByType<RE::ExtraUniqueID>();
		if (!id || id->baseID == 0) {
			return std::nullopt;
		}
		return core::StaffKey{ a_held.staff->GetFormID(), id->baseID, id->uniqueID };
	}

	std::string DisplayName(const Held& a_held)
	{
		const auto* name = a_held.list ? a_held.list->GetDisplayName(a_held.staff) : nullptr;
		return name ? name : "";
	}

	bool HasOwnName(const Held& a_held)
	{
		return a_held.list && a_held.list->HasType(RE::ExtraDataType::kTextDisplayData);
	}

	bool NameLocked(const Held& a_held)
	{
		const auto* text = a_held.list ? a_held.list->GetByType<RE::ExtraTextDisplayData>() : nullptr;
		return text && (text->displayNameText || text->ownerQuest);
	}

	bool WriteName(const Held& a_held, const std::string& a_name)
	{
		if (!a_held.list) {
			return false;
		}
		a_held.list->SetOverrideName(a_name.c_str());
		return DisplayName(a_held) == a_name;
	}

	const char* RestoreName(const Held& a_held, const core::NameState& a_state)
	{
		switch (core::DecideRestore(a_state, DisplayName(a_held))) {
		case core::NameRestore::kRestoreOriginal:
			a_held.list->SetOverrideName(a_state.original.c_str());
			return "own name restored";
		case core::NameRestore::kRemoveOverride:
			a_held.list->RemoveByType(RE::ExtraDataType::kTextDisplayData);
			return "our name removed";
		case core::NameRestore::kLeave:
			break;
		}
		return "renamed by someone else: left as is";
	}

	RE::MagicItem* OwnEnchantment(const Held& a_held)
	{
		if (a_held.staff && a_held.staff->formEnchanting) {
			return a_held.staff->formEnchanting;
		}
		const auto* extra = a_held.list ? a_held.list->GetByType<RE::ExtraEnchantment>() : nullptr;
		return extra ? extra->enchantment : nullptr;
	}
}
