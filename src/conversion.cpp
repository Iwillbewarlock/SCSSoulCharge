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

		bool IsConvertedStaff(const RE::TESForm* a_form)
		{
			return a_form && a_form->Is(RE::FormType::Weapon) && keywords::IsConverted(a_form->GetFormID());
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

		// The staff's own enchantment, ignoring SCS's shared one we may have attached.
		RE::EnchantmentItem* OwnEnchantment(const RE::TESObjectWEAP* a_weapon)
		{
			return a_weapon ? a_weapon->formEnchanting : nullptr;
		}

		bool HandUses(RE::PlayerCharacter* a_player, bool a_left, const RE::EnchantmentItem* a_enchantment)
		{
			auto* held = a_player->GetEquippedObject(a_left);
			return IsConvertedStaff(held) && OwnEnchantment(held->As<RE::TESObjectWEAP>()) == a_enchantment;
		}

		void ConvertWorn(RE::PlayerCharacter* a_player, RE::TESObjectWEAP* a_weapon, bool a_left)
		{
			auto* shared = pool::SharedEnchantment();
			auto* own = OwnEnchantment(a_weapon);
			const auto worn = FindWorn(a_player, a_weapon, a_left);
			if (!shared || !own || !worn.list) {
				logger::warn("CONVERT {} {:08X}: worn item not found", a_left ? "L" : "R", a_weapon->GetFormID());
				return;
			}
			if (const auto* existing = worn.list->GetByType<RE::ExtraEnchantment>(); existing && existing->enchantment != shared) {
				logger::info("CONVERT {} {:08X}: player-made enchantment {:08X}, left alone", a_left ? "L" : "R",
					a_weapon->GetFormID(), existing->enchantment ? existing->enchantment->GetFormID() : 0u);
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
			selftest::Report("T-C2a", resolved == shared,
				std::format("hand={} weapon={:08X} entryEnchantment={:08X} shared={:08X} spell={:08X} learned={}",
					a_left ? "L" : "R", a_weapon->GetFormID(), resolved ? resolved->GetFormID() : 0u, shared->GetFormID(),
					spell->GetFormID(), a_player->HasSpell(spell)));
		}

		void ReleaseSpell(RE::PlayerCharacter* a_player, RE::TESObjectWEAP* a_weapon)
		{
			auto* own = OwnEnchantment(a_weapon);
			auto* spell = pool::FindSpell(own);
			if (!spell || !a_player->HasSpell(spell)) {
				return;
			}
			if (HandUses(a_player, true, own) || HandUses(a_player, false, own)) {
				return;  // a hand still holds a staff with this enchantment
			}
			a_player->RemoveSpell(spell);
			logger::info("RELEASE {:08X}: charge spell {:08X} removed", a_weapon->GetFormID(), spell->GetFormID());
		}

		// Restores the staff's own enchantment on every copy of a_base inside a_holder.
		std::uint32_t RevertIn(RE::ExtraDataList* a_list, const RE::EnchantmentItem* a_shared)
		{
			auto* existing = a_list ? a_list->GetByType<RE::ExtraEnchantment>() : nullptr;
			if (!existing || existing->enchantment != a_shared) {
				return 0;
			}
			a_list->RemoveByType(RE::ExtraDataType::kEnchantment);
			return 1;
		}

		std::uint32_t RevertInContainer(RE::TESObjectREFR* a_container, RE::FormID a_base, const RE::EnchantmentItem* a_shared)
		{
			auto* changes = a_container ? a_container->GetInventoryChanges() : nullptr;
			if (!changes || !changes->entryList) {
				return 0;
			}
			std::uint32_t reverted = 0;
			for (auto* entry : *changes->entryList) {
				if (!entry || !entry->object || entry->object->GetFormID() != a_base || !entry->extraLists) {
					continue;
				}
				for (auto* list : *entry->extraLists) {
					reverted += RevertIn(list, a_shared);
				}
			}
			return reverted;
		}

		class Sink :
			public RE::BSTEventSink<RE::TESEquipEvent>,
			public RE::BSTEventSink<RE::TESContainerChangedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				if (!a_event || !a_event->actor || !a_event->actor->IsPlayerRef() || !pool::IsReady()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto* player = RE::PlayerCharacter::GetSingleton();
				auto* weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(a_event->baseObject);
				if (!player || !IsConvertedStaff(weapon)) {
					return RE::BSEventNotifyControl::kContinue;
				}
				if (a_event->equipped) {
					if (g_blocked) {
						logger::warn("CONVERT {:08X} skipped: conversion blocked for this save", weapon->GetFormID());
						return RE::BSEventNotifyControl::kContinue;
					}
					for (const bool left : { true, false }) {
						if (player->GetEquippedObject(left) == weapon) {
							ConvertWorn(player, weapon, left);
						}
					}
				} else {
					ReleaseSpell(player, weapon);
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				if (!a_event || a_event->oldContainer != kPlayerRef || a_event->newContainer == kPlayerRef) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto* weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(a_event->baseObj);
				const auto* shared = pool::SharedEnchantment();
				if (!IsConvertedStaff(weapon) || !shared) {
					return RE::BSEventNotifyControl::kContinue;
				}
				std::uint32_t reverted = 0;
				if (auto dropped = a_event->reference.get()) {
					reverted += RevertIn(&dropped->extraList, shared);
				}
				if (a_event->newContainer) {
					reverted += RevertInContainer(RE::TESForm::LookupByID<RE::TESObjectREFR>(a_event->newContainer), a_event->baseObj, shared);
				}
				logger::info("LEAVE {:08X} -> {:08X} x{}: {} item(s) got their own enchantment back", a_event->baseObj,
					a_event->newContainer, a_event->itemCount, reverted);
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
		}
		g_blocked = a_blocked;
		if (a_blocked) {
			RE::SendHUDMessage::ShowHUDMessage("SCSSoulCharge: staff conversion paused for this save (see SCSSoulCharge.log)");
		}
	}

	bool IsBlocked() { return g_blocked; }

	void ReconcilePlayer()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !pool::IsReady() || g_blocked) {
			return;
		}
		for (const bool left : { true, false }) {
			auto* weapon = player->GetEquippedObject(left);
			if (IsConvertedStaff(weapon)) {
				ConvertWorn(player, weapon->As<RE::TESObjectWEAP>(), left);
			}
		}
		// Charge spells the player still knows without holding a matching staff (e.g. saved mid-swap).
		for (const auto& entry : pool::Table().Entries()) {
			auto* enchantment = entry.enchantment ? RE::TESForm::LookupByID<RE::EnchantmentItem>(entry.enchantment) : nullptr;
			auto* spell = pool::FindSpell(enchantment);
			if (spell && player->HasSpell(spell) && !HandUses(player, true, enchantment) && !HandUses(player, false, enchantment)) {
				player->RemoveSpell(spell);
				logger::info("RECONCILE: charge spell {:08X} removed (no staff in hand uses it)", spell->GetFormID());
			}
		}
	}
}
