#include "PCH.h"

#include "conversion.h"

#include "keywords.h"
#include "pool.h"
#include "selftest.h"
#include "watch.h"

#include <unordered_map>
#include <unordered_set>

namespace scssc::conversion
{
	namespace
	{
		constexpr RE::FormID kPlayerRef = 0x14;

		bool g_blocked = false;
		// Per-item names (ExtraTextDisplayData) seen when a staff was converted; SCS overwrites them (Naming.cpp:20-38).
		// Keyed by the item's extra list, which stays the same object while the item is held.
		std::unordered_map<const RE::ExtraDataList*, std::string> g_itemNames;
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
			if (!a_base || !a_list || !a_shared || (a_base->formEnchanting == a_shared && !keywords::IsConverted(a_base->GetFormID()))) {
				return false;
			}
			const auto* extra = a_list->GetByType<RE::ExtraEnchantment>();
			return extra && extra->enchantment == a_shared;
		}

		// The staff's own enchantment, never SCS's shared one even while the record is swapped.
		RE::EnchantmentItem* OwnEnchantment(const RE::TESObjectWEAP* a_weapon)
		{
			if (!a_weapon) {
				return nullptr;
			}
			auto* original = keywords::OriginalEnchantment(a_weapon->GetFormID());
			return original ? original : a_weapon->formEnchanting;
		}

		// In-game test (2026-11-06): the hand casts the enchantment on the staff RECORD, ignoring the item's
		// ExtraEnchantment and the GetEnchantment lookup. So while the player holds a converted staff, its
		// record points at SCS's shared enchantment; it goes back as soon as no hand holds that staff.
		// Side effect for that time: NPCs using the same staff type cast the empty shared enchantment.
		std::unordered_set<RE::TESObjectWEAP*> g_swapped;

		void SwapRecord(RE::TESObjectWEAP* a_weapon, RE::EnchantmentItem* a_shared)
		{
			if (a_weapon && a_shared && a_weapon->formEnchanting != a_shared) {
				a_weapon->formEnchanting = a_shared;
				g_swapped.insert(a_weapon);
				logger::info("SWAP {:08X}: record enchantment -> shared while held", a_weapon->GetFormID());
			}
		}

		void RestoreRecord(RE::TESObjectWEAP* a_weapon)
		{
			if (auto* original = keywords::OriginalEnchantment(a_weapon->GetFormID())) {
				a_weapon->formEnchanting = original;
			}
			logger::info("SWAP {:08X}: record enchantment restored", a_weapon->GetFormID());
		}

		// Puts back every swapped record no hand holds (or all of them when a_all).
		void RestoreRecords(RE::PlayerCharacter* a_player, bool a_all)
		{
			for (auto it = g_swapped.begin(); it != g_swapped.end();) {
				auto* weapon = *it;
				const bool held = !a_all && a_player &&
				                  (a_player->GetEquippedObject(true) == weapon || a_player->GetEquippedObject(false) == weapon);
				if (held) {
					++it;
					continue;
				}
				RestoreRecord(weapon);
				it = g_swapped.erase(it);
			}
		}

		struct WornItem
		{
			RE::InventoryEntryData* entry{ nullptr };
			RE::ExtraDataList*      list{ nullptr };
		};

		// Bumped by every equip / container event and by Revert; a cached worn item is valid only
		// within one generation. Keeps the per-frame callers (names, channel, self-tests) off a full
		// inventory scan while nothing changed.
		std::uint64_t g_inventoryGeneration = 0;

		struct WornCache
		{
			const RE::TESBoundObject* base{ nullptr };
			std::uint64_t             generation{ ~0ull };
			WornItem                  item;
		};
		std::array<WornCache, 2> g_wornCache{};  // 0 = left

		WornItem ScanWorn(RE::PlayerCharacter* a_player, const RE::TESBoundObject* a_base, bool a_left)
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

		WornItem FindWorn(RE::PlayerCharacter* a_player, const RE::TESBoundObject* a_base, bool a_left)
		{
			auto& cache = g_wornCache[a_left ? 0 : 1];
			if (cache.base == a_base && cache.generation == g_inventoryGeneration) {
				return cache.item;
			}
			cache = { a_base, g_inventoryGeneration, ScanWorn(a_player, a_base, a_left) };
			return cache.item;
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
			return IsOurConversion(base, worn.list, pool::SharedEnchantment()) ? pool::FindSpell(OwnEnchantment(base)) : nullptr;
		}

		void ConvertWorn(RE::PlayerCharacter* a_player, RE::TESObjectWEAP* a_weapon, bool a_left)
		{
			auto*      shared = pool::SharedEnchantment();
			auto*      own = OwnEnchantment(a_weapon);
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
			if (const auto* custom = worn.list->GetByType<RE::ExtraTextDisplayData>(); custom && custom->displayName.c_str()) {
				g_itemNames.try_emplace(worn.list, custom->displayName.c_str());  // before SCS can overwrite it
			}
			if (!worn.list->HasType(RE::ExtraDataType::kEnchantment)) {
				worn.list->SetEnchantment(shared, static_cast<std::uint16_t>(a_weapon->amountofEnchantment), false);
				logger::info("CONVERT {} {:08X} '{}': shared ENCH attached, maxCharge={}", a_left ? "L" : "R",
					a_weapon->GetFormID(), a_weapon->GetName(), a_weapon->amountofEnchantment);
			}
			SwapRecord(a_weapon, shared);
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
			RestoreRecords(a_player, false);
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

		void HandleEquip(RE::FormID a_baseID, bool a_equipped)
		{
			++g_inventoryGeneration;  // the equip transaction finished since the event
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || !g_settled) {
				return;
			}
			if (a_equipped) {
				GuardBareHands(player);
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
			if (!a_equipped) {
				watch::OnUnequip(player, weapon);
			}
			ReleaseUnbackedSpells(player);
		}

		constexpr float kDropSearchRadius = 512.0f;

		void RevertLeftItems(RE::FormID a_baseID, RE::FormID a_newContainer, std::int32_t a_count, RE::ObjectRefHandle a_handle)
		{
			const auto* weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(a_baseID);
			const auto* shared = pool::SharedEnchantment();
			auto*       player = RE::PlayerCharacter::GetSingleton();
			if (!weapon || !shared || !player) {
				return;
			}
			std::uint32_t reverted = 0;
			if (auto dropped = a_handle.get()) {
				reverted += RevertList(weapon, &dropped->extraList, shared);
			}
			if (a_newContainer) {
				reverted += RevertInInventory(RE::TESForm::LookupByID<RE::TESObjectREFR>(a_newContainer), weapon, shared, false);
			} else if (reverted == 0) {
				// Dropped into the world without a usable handle: look for our converted copies of this base near the player.
				if (auto* cell = player->GetParentCell()) {
					cell->ForEachReferenceInRange(player->GetPosition(), kDropSearchRadius, [&](RE::TESObjectREFR* a_ref) {
						if (a_ref && a_ref->GetBaseObject() == weapon) {
							reverted += RevertList(weapon, &a_ref->extraList, shared);
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
				}
			}
			logger::info("LEAVE {:08X} -> {:08X} x{}: {} item(s) got their own enchantment back", a_baseID, a_newContainer, a_count, reverted);
			const auto detail = std::format("base={:08X} newContainer={:08X} count={} reverted={}", a_baseID, a_newContainer, a_count, reverted);
			if (reverted > 0) {
				selftest::Report("T-REV", true, detail);
			} else {
				selftest::Skip("T-REV", detail + " (the item may never have been converted; check it with ObjectReference.GetEnchantment)");
			}
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
				++g_inventoryGeneration;
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
						SwapRecord(weapon, pool::SharedEnchantment());
						if (auto* spell = pool::SpellFor(OwnEnchantment(weapon)); spell && !player->HasSpell(spell)) {
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
				if (a_event && (a_event->oldContainer == kPlayerRef || a_event->newContainer == kPlayerRef)) {
					++g_inventoryGeneration;  // the player's inventory lists may have been rebuilt
				}
				if (!a_event || a_event->oldContainer != kPlayerRef || a_event->newContainer == kPlayerRef || !g_settled) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto* weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(a_event->baseObj);
				const auto* shared = pool::SharedEnchantment();
				if (!weapon || !shared || (weapon->formEnchanting == shared && !keywords::IsConverted(weapon->GetFormID()))) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// In-game test (case 281): a dropped staff kept the shared ENCH when handled inside the event,
				// so the item is looked up one task later, when the move has finished.
				const auto baseID = a_event->baseObj;
				const auto newContainer = a_event->newContainer;
				const auto count = a_event->itemCount;
				const auto handle = a_event->reference;
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([baseID, newContainer, count, handle] { RevertLeftItems(baseID, newContainer, count, handle); });
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

	void SetSettled(bool a_settled)
	{
		if (!a_settled) {
			RestoreRecords(nullptr, true);  // a load starts: no record stays swapped across saves
		}
		g_settled = a_settled;
		++g_inventoryGeneration;  // a load or new game replaces every inventory list
	}

	// A charge spell put straight into a hand (no staff there to intercept it, SCS Equipment.cpp:38-42)
	// would cast for free through the normal hand pipeline, which never reaches the cast hook.
	void GuardBareHands(RE::PlayerCharacter* a_player)
	{
		for (const bool left : { true, false }) {
			auto* form = a_player->GetEquippedObject(left);
			auto* spell = form ? form->As<RE::SpellItem>() : nullptr;
			if (!spell || !pool::IsPoolSpell(spell)) {
				continue;
			}
			a_player->DeselectSpell(spell);
			const bool removed = a_player->GetEquippedObject(left) != spell;
			// Runs every frame: report a stuck spell once per 2 s, not 60 times a second.
			static auto lastReport = std::chrono::steady_clock::time_point{};
			const auto  now = std::chrono::steady_clock::now();
			if (removed || now - lastReport >= std::chrono::seconds(2)) {
				lastReport = now;
				logger::info("BARE {} charge spell {:08X} unequipped from hand: {}", left ? "L" : "R", spell->GetFormID(), removed);
				selftest::Report("T-BARE", removed, std::format("hand={} spell={:08X} stillEquipped={}", left ? "L" : "R", spell->GetFormID(), !removed));
				RE::SendHUDMessage::ShowHUDMessage("Charge spells can only be cast through their staff");
			}
		}
	}

	bool RestoreItemName(RE::PlayerCharacter* a_player, bool a_left)
	{
		auto* weapon = a_player->GetEquippedObject(a_left);
		auto* base = weapon ? weapon->As<RE::TESObjectWEAP>() : nullptr;
		if (!base) {
			return false;
		}
		const auto worn = FindWorn(a_player, base, a_left);
		const auto it = worn.list ? g_itemNames.find(worn.list) : g_itemNames.end();
		auto*      custom = worn.list ? worn.list->GetByType<RE::ExtraTextDisplayData>() : nullptr;
		if (it == g_itemNames.end() || !custom) {
			return false;
		}
		const char* now = custom->displayName.c_str();
		if (now && it->second == now) {
			return false;
		}
		custom->SetName(it->second.c_str());
		return true;
	}

	const RE::ExtraDataList* WornList(RE::PlayerCharacter* a_player, bool a_left)
	{
		auto* weapon = a_player->GetEquippedObject(a_left);
		auto* base = weapon ? weapon->As<RE::TESBoundObject>() : nullptr;
		return base ? FindWorn(a_player, base, a_left).list : nullptr;
	}

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
		g_itemNames.clear();  // extra lists of the previous save are gone
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
		GuardBareHands(player);
	}
}
