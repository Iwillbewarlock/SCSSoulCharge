#include "PCH.h"

#include "conversion.h"

#include "keywords.h"
#include "pool.h"
#include "selftest.h"

namespace scssc::conversion
{
	namespace
	{
		constexpr RE::FormID kPlayerRef = 0x14;

		bool g_blocked = false;
		bool g_settled = false;  // false between Revert and kPostLoadGame/kNewGame: inventory is being rebuilt

		// A staff this run converts (base got SCS keywords from us).
		bool IsConvertibleStaff(const RE::TESForm* a_form)
		{
			return a_form && a_form->Is(RE::FormType::Weapon) && keywords::IsConverted(a_form->GetFormID());
		}

		// An item we converted at some point: SCS's shared ENCH on a weapon that is not one of SCS's own
		// staves (those carry the shared ENCH on the base record). Independent of this run's INI.
		bool IsOurConversion(const RE::TESObjectWEAP* a_base, const RE::ExtraDataList* a_list, const RE::EnchantmentItem* a_shared)
		{
			if (!a_base || !a_list || !a_shared || a_base->formEnchanting == a_shared) {
				return false;
			}
			const auto* extra = a_list->GetByType<RE::ExtraEnchantment>();
			return extra && extra->enchantment == a_shared;
		}

		struct WornItem
		{
			RE::InventoryEntryData* entry{ nullptr };
			RE::ExtraDataList*      list{ nullptr };
		};

		WornItem FindWorn(RE::PlayerCharacter* a_player, const RE::TESBoundObject* a_base, bool a_left)
		{
			auto* changes = a_player->GetInventoryChanges();
			if (!changes || !changes->entryList) {
				return {};
			}
			const auto wornType = a_left ? RE::ExtraDataType::kWornLeft : RE::ExtraDataType::kWorn;
			for (auto* entry : *changes->entryList) {
				if (!entry || entry->object != a_base || !entry->extraLists) {
					continue;
				}
				for (auto* list : *entry->extraLists) {
					if (list && list->HasType(wornType)) {
						return { entry, list };
					}
				}
			}
			return {};
		}

		// The charge spell a hand currently backs: a converted staff in that hand, carrying the shared ENCH.
		RE::SpellItem* SpellBackedByHand(RE::PlayerCharacter* a_player, bool a_left)
		{
			auto* weapon = a_player->GetEquippedObject(a_left);
			auto* base = weapon ? weapon->As<RE::TESObjectWEAP>() : nullptr;
			if (!IsConvertibleStaff(base)) {
				return nullptr;
			}
			const auto worn = FindWorn(a_player, base, a_left);
			return IsOurConversion(base, worn.list, pool::SharedEnchantment()) ? pool::FindSpell(base->formEnchanting) : nullptr;
		}

		void ConvertWorn(RE::PlayerCharacter* a_player, RE::TESObjectWEAP* a_weapon, bool a_left)
		{
			auto*      shared = pool::SharedEnchantment();
			auto*      own = a_weapon->formEnchanting;
			const auto worn = FindWorn(a_player, a_weapon, a_left);
			if (!shared || !own || !worn.list) {
				logger::warn("CONVERT {} {:08X}: worn item not found", a_left ? "L" : "R", a_weapon->GetFormID());
				return;
			}
			if (const auto* existing = worn.list->GetByType<RE::ExtraEnchantment>(); existing && existing->enchantment != shared) {
				// SCS still treats this copy as a catalyst (it checks the base keyword only, Equipment.cpp:261-278),
				// but we never overwrite an enchantment the player or another mod put on the item.
				logger::warn("CONVERT {} {:08X}: item has its own extra enchantment {:08X}; not converted, SCS will not cast with it",
					a_left ? "L" : "R", a_weapon->GetFormID(), existing->enchantment ? existing->enchantment->GetFormID() : 0u);
				RE::SendHUDMessage::ShowHUDMessage("SCSSoulCharge: this staff has a custom enchantment and cannot be used as a catalyst");
				return;
			}
			auto* spell = pool::SpellFor(own);
			if (!spell) {
				return;
			}
			if (!worn.list->HasType(RE::ExtraDataType::kEnchantment)) {
				worn.list->SetEnchantment(shared, static_cast<std::uint16_t>(a_weapon->amountofEnchantment), false);
				logger::info("CONVERT {} {:08X} '{}': shared ENCH attached, maxCharge={}", a_left ? "L" : "R",
					a_weapon->GetFormID(), a_weapon->GetName(), a_weapon->amountofEnchantment);
			}
			if (!a_player->HasSpell(spell)) {
				a_player->AddSpell(spell);
			}
			const auto* resolved = worn.entry->GetEnchantment();
			const auto* listExtra = worn.list->GetByType<RE::ExtraEnchantment>();
			selftest::Report("T-C2a", resolved == shared && listExtra && listExtra->enchantment == shared,
				std::format("hand={} weapon={:08X} entryEnchantment={:08X} wornListEnchantment={:08X} shared={:08X} spell={:08X} learned={}",
					a_left ? "L" : "R", a_weapon->GetFormID(), resolved ? resolved->GetFormID() : 0u,
					listExtra && listExtra->enchantment ? listExtra->enchantment->GetFormID() : 0u, shared->GetFormID(),
					spell->GetFormID(), a_player->HasSpell(spell)));
		}

		// Forget every charge spell no hand backs right now (covers spells of vanished enchantments too).
		void ReleaseUnbackedSpells(RE::PlayerCharacter* a_player)
		{
			const auto* left = SpellBackedByHand(a_player, true);
			const auto* right = SpellBackedByHand(a_player, false);
			for (auto* spell : pool::Spells()) {
				if (spell != left && spell != right && a_player->HasSpell(spell)) {
					a_player->RemoveSpell(spell);
					logger::info("RELEASE charge spell {:08X} (no staff in hand uses it)", spell->GetFormID());
				}
			}
		}

		std::uint32_t RevertList(const RE::TESObjectWEAP* a_base, RE::ExtraDataList* a_list, const RE::EnchantmentItem* a_shared)
		{
			if (!IsOurConversion(a_base, a_list, a_shared)) {
				return 0;
			}
			a_list->RemoveByType(RE::ExtraDataType::kEnchantment);
			return 1;
		}

		// Restores the own enchantment on every converted copy of a_base (or of any weapon when a_base is null).
		std::uint32_t RevertInInventory(RE::TESObjectREFR* a_holder, const RE::TESObjectWEAP* a_base, const RE::EnchantmentItem* a_shared,
			bool a_skipWorn)
		{
			auto* changes = a_holder ? a_holder->GetInventoryChanges() : nullptr;
			if (!changes || !changes->entryList) {
				return 0;
			}
			std::uint32_t reverted = 0;
			for (auto* entry : *changes->entryList) {
				auto* base = entry && entry->object ? entry->object->As<RE::TESObjectWEAP>() : nullptr;
				if (!base || (a_base && base != a_base) || !entry->extraLists) {
					continue;
				}
				for (auto* list : *entry->extraLists) {
					if (list && !(a_skipWorn && list->GetWorn())) {
						reverted += RevertList(base, list, a_shared);
					}
				}
			}
			return reverted;
		}

		// A charge spell put straight into a hand (no staff there to intercept it, SCS Equipment.cpp:38-42)
		// would cast for free through the normal hand pipeline, which never reaches our cast hook.
		void GuardBareHands(RE::PlayerCharacter* a_player, RE::FormID a_spellID)
		{
			auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(a_spellID);
			if (!pool::IsPoolSpell(spell)) {
				return;
			}
			for (const bool left : { true, false }) {
				if (a_player->GetEquippedObject(left) == spell) {
					a_player->DeselectSpell(spell);
					const bool removed = a_player->GetEquippedObject(left) != spell;
					logger::info("BARE {} charge spell {:08X} unequipped from hand: {}", left ? "L" : "R", a_spellID, removed);
					selftest::Report("T-BARE", removed, std::format("hand={} spell={:08X} stillEquipped={}", left ? "L" : "R", a_spellID, !removed));
					RE::SendHUDMessage::ShowHUDMessage("Charge spells can only be cast through their staff");
				}
			}
		}

		void HandleEquip(RE::FormID a_baseID, bool a_equipped)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || !g_settled) {
				return;
			}
			if (a_equipped) {
				GuardBareHands(player, a_baseID);
			}
			auto* weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(a_baseID);
			if (!weapon) {
				return;
			}
			if (a_equipped && IsConvertibleStaff(weapon)) {
				if (g_blocked) {
					logger::warn("CONVERT {:08X} skipped: conversion blocked for this save", a_baseID);
				} else {
					bool matched = false;
					for (const bool left : { true, false }) {
						if (player->GetEquippedObject(left) == weapon) {
							matched = true;
							ConvertWorn(player, weapon, left);
						}
					}
					if (!matched) {
						logger::warn("CONVERT {:08X}: equip event but no hand holds it one frame later", a_baseID);
					}
				}
			}
			ReleaseUnbackedSpells(player);
		}

		class Sink :
			public RE::BSTEventSink<RE::TESEquipEvent>,
			public RE::BSTEventSink<RE::TESContainerChangedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				if (!a_event || !a_event->actor || !a_event->actor->IsPlayerRef() || !pool::IsReady() || !g_settled) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto baseID = a_event->baseObject;
				const bool equipped = a_event->equipped;
				// Learn the charge spell now, inside the event: SCS only queues its Sync here (Events.cpp:19-20)
				// and runs it on a later frame, so the spell is known when SCS restores the staff's binding.
				// Attaching the enchantment waits for the task below, when the hand state is final; that task
				// also forgets the spell again if no hand ends up holding the staff.
				if (equipped && !g_blocked) {
					auto* weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(baseID);
					auto* player = RE::PlayerCharacter::GetSingleton();
					if (player && IsConvertibleStaff(weapon)) {
						if (auto* spell = pool::SpellFor(weapon->formEnchanting); spell && !player->HasSpell(spell)) {
							player->AddSpell(spell);
						}
					}
				}
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([baseID, equipped] { HandleEquip(baseID, equipped); });
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				if (!a_event || a_event->oldContainer != kPlayerRef || a_event->newContainer == kPlayerRef || !g_settled) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto* weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(a_event->baseObj);
				const auto* shared = pool::SharedEnchantment();
				if (!weapon || !shared || weapon->formEnchanting == shared) {
					return RE::BSEventNotifyControl::kContinue;
				}
				std::uint32_t reverted = 0;
				if (auto dropped = a_event->reference.get()) {
					reverted += RevertList(weapon, &dropped->extraList, shared);
				}
				if (a_event->newContainer) {
					reverted += RevertInInventory(RE::TESForm::LookupByID<RE::TESObjectREFR>(a_event->newContainer), weapon, shared, false);
				}
				if (reverted) {
					logger::info("LEAVE {:08X} -> {:08X} x{}: {} item(s) got their own enchantment back", a_event->baseObj,
						a_event->newContainer, a_event->itemCount, reverted);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		Sink g_sink;
	}

	void Install()
	{
		auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
		if (!holder) {
			logger::critical("Event sources unavailable; staves are not converted");
			return;
		}
		holder->AddEventSink<RE::TESEquipEvent>(&g_sink);
		holder->AddEventSink<RE::TESContainerChangedEvent>(&g_sink);
	}

	void SetBlocked(bool a_blocked, std::string_view a_reason)
	{
		if (g_blocked != a_blocked) {
			logger::warn("Conversion {} ({})", a_blocked ? "BLOCKED" : "unblocked", a_reason);
			if (a_blocked) {
				RE::SendHUDMessage::ShowHUDMessage("SCSSoulCharge: staff conversion paused for this save (see SCSSoulCharge.log)");
			}
		}
		g_blocked = a_blocked;
	}

	bool IsBlocked() { return g_blocked; }

	void SetSettled(bool a_settled) { g_settled = a_settled; }

	std::optional<bool> HandForSpell(RE::PlayerCharacter* a_player, const RE::SpellItem* a_spell)
	{
		for (const bool left : { true, false }) {
			if (a_spell && SpellBackedByHand(a_player, left) == a_spell) {
				return left;
			}
		}
		return std::nullopt;
	}

	void ReconcilePlayer()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* shared = pool::SharedEnchantment();
		if (!player || !pool::IsReady() || !shared) {
			return;
		}
		// Converted items this run no longer converts (INI changed) get their own enchantment back.
		auto* changes = player->GetInventoryChanges();
		std::uint32_t reverted = 0;
		if (changes && changes->entryList) {
			for (auto* entry : *changes->entryList) {
				auto* base = entry && entry->object ? entry->object->As<RE::TESObjectWEAP>() : nullptr;
				if (base && !IsConvertibleStaff(base) && entry->extraLists) {
					for (auto* list : *entry->extraLists) {
						reverted += RevertList(base, list, shared);
					}
				}
			}
		}
		if (reverted) {
			logger::info("RECONCILE: {} item(s) no longer converted got their own enchantment back", reverted);
		}
		if (!g_blocked) {
			for (const bool left : { true, false }) {
				auto* weapon = player->GetEquippedObject(left);
				if (IsConvertibleStaff(weapon)) {
					ConvertWorn(player, weapon->As<RE::TESObjectWEAP>(), left);
				}
			}
		}
		ReleaseUnbackedSpells(player);
	}
}
