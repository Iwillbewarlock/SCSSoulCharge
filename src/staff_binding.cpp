#include "staff_binding.h"

#include "frame_hook.h"
#include "hand.h"
#include "hand_slot.h"
#include "main_queue.h"
#include "messages.h"
#include "selftest.h"
#include "settings.h"
#include "spell_copy.h"
#include "staff_item.h"
#include "staff_mark.h"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace scssc::staff_binding
{
	namespace
	{
		// The menu's equip slots (SCS Controller.cpp:135). Only these two pick a hand; any other slot (both hands for a
		// ritual spell, none) goes to the game as usual (Astra 2026-11-20).
		constexpr RE::FormID kRightHandSlot = 0x13F42;
		constexpr RE::FormID kLeftHandSlot = 0x13F43;

		using MenuEquipFn = void (*)(RE::ActorEquipManager*, RE::Actor*, RE::SpellItem*, RE::BGSEquipSlot**);
		MenuEquipFn g_previousMenuEquip = nullptr;

		core::BindingTable g_table;  // main thread (frame hook, and the co-save callbacks: T-THREAD checks that)

		// Counts the player's weapon equip changes. A queued pick made before one is dropped (design 5): another staff
		// of the same kind may be in the hand now. Picks themselves do not count, so two picks in one menu (assign, then
		// clear) both apply in order; equipping a spell or armor in the same menu does not cancel a pick.
		std::atomic<std::uint32_t> g_weaponSeq{ 0 };

		// Unique id changes, applied at the next frame or right before the co-save is written, whichever comes first.
		std::mutex                                             g_moveLock;
		std::vector<std::pair<core::StaffKey, core::StaffKey>> g_pendingMoves;

		// Set while the save swap holds the staff's own enchantment in the slot; nothing else writes the slot then.
		std::atomic<bool> g_saving{ false };

		// Per hand: the held binding whose spell is not known right now, so its notice is shown once per change.
		std::array<std::optional<core::StaffKey>, 2> g_dormant;

		// What the engine had put in the hand slot for each staff before our first copy (the test build restored the
		// slot's own value, a560bf0 testcontrols.cpp:1378). Forms, so the pointers stay valid; cleared on load.
		std::map<core::StaffKey, RE::MagicItem*> g_engineOwn;

		// After a load with bindings: the first copy written for a held bound staff proves the save round trip.
		bool g_rebindPending = false;

		std::array<RE::SpellItem*, 2> g_activeSpell{};  // main thread, per hand; what staff_mark marks

		// Why saved bindings are kept but not applied: the menu hook could not be placed (nothing could change or clear
		// them, Astra 2026-11-19), or the [Uninstall] option runs. The staff casts its own enchantment meanwhile.
		enum class Off : std::uint8_t
		{
			kNo,
			kMenuHook,
			kUninstall,
		};
		std::atomic<Off> g_off{ Off::kNo };
		bool             g_menuHooked = false;  // the menu hook result, once placed or refused

		// Names left by bindings whose item lost its unique id where we could not reach it (main thread, this game run):
		// the name is taken off when the item shows up again, so a new pick does not stack "[A] [B]" (Carbon 2026-11-19).
		std::vector<std::pair<RE::FormID, core::NameState>> g_lostNames;

		std::string KeyText(const core::StaffKey& a_key)
		{
			return std::format("{:08X}/({:08X},{})", a_key.staff, a_key.uidBase, a_key.uid);
		}

		void Notify(const std::string& a_text)
		{
			logger::info("notify: {}", a_text);
			RE::SendHUDMessage::ShowHUDMessage(a_text.c_str());
		}

		// The item lost its id: give its name back now if it is still in the player's inventory, else remember it.
		void RestoreLostName(RE::FormID a_staff, const core::NameState& a_name)
		{
			if (a_name.written.empty()) {
				return;
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (const auto held = staff_item::FindWithoutId(player, a_staff, a_name.written)) {
				logger::info("lost-id staff {:08X}: {}", a_staff, staff_item::RestoreName(*held, a_name));
				return;
			}
			logger::warn("lost-id staff {:08X} is not in the inventory; its name '{}' goes back when it is held again (this game run only)", a_staff, a_name.written);
			g_lostNames.emplace_back(a_staff, a_name);
		}

		// A held staff without a binding that still shows a name we wrote before its id was lost.
		void TakeLostName(const staff_item::Held& a_held)
		{
			if (g_lostNames.empty()) {
				return;
			}
			const auto shown = staff_item::DisplayName(a_held);
			const auto it = std::ranges::find_if(g_lostNames, [&](const auto& a_entry) {
				return a_entry.first == a_held.staff->GetFormID() && a_entry.second.written == shown;
			});
			if (it != g_lostNames.end()) {
				logger::info("lost-id staff {:08X} held again: {}", it->first, staff_item::RestoreName(a_held, it->second));
				g_lostNames.erase(it);
			}
		}

		void ApplyPendingMoves()
		{
			std::vector<std::pair<core::StaffKey, core::StaffKey>> moves;
			{
				const std::scoped_lock lock{ g_moveLock };
				moves.swap(g_pendingMoves);
			}
			for (const auto& [from, to] : moves) {
				if (from.uidBase == 0) {
					continue;  // an id given to an item that had none: nothing can be bound under {staff, 0, 0} (Carbon 2026-11-17)
				}
				if (to.uidBase == 0) {
					if (const auto* binding = g_table.Find(from)) {
						const auto name = binding->name;
						g_table.Remove(from);
						logger::info("binding {} dropped: the item lost its id", KeyText(from));
						RestoreLostName(from.staff, name);
					}
					g_engineOwn.erase(from);
					continue;
				}
				const bool bound = g_table.Find(from) != nullptr;
				if (g_table.Move(from, to)) {
					if (auto it = g_engineOwn.find(from); it != g_engineOwn.end()) {
						g_engineOwn[to] = it->second;
						g_engineOwn.erase(it);
					}
					logger::info("binding moved with the item's new id: {} -> {}", KeyText(from), KeyText(to));
				}
				if (bound) {
					selftest::Report("T-UNIQ", g_table.Find(to) != nullptr, std::format("{} -> {}", KeyText(from), KeyText(to)));
				}
			}
		}

		std::optional<staff_item::Held> InHand(RE::PlayerCharacter* a_player, Hand a_hand)
		{
			return a_hand == Hand::kRight ? staff_item::RightHand(a_player) : staff_item::LeftHand(a_player);
		}

		bool CasterIdle(RE::PlayerCharacter* a_player, Hand a_hand)
		{
			const auto* caster = Caster(a_player, a_hand);
			return !caster || caster->state.underlying() == 0;
		}

		// The enchantment to put back for this staff: the engine's own slot value when we saw it, else the record's.
		RE::MagicItem* OwnFor(const staff_item::Held& a_held, const std::optional<core::StaffKey>& a_key)
		{
			if (a_key) {
				if (const auto it = g_engineOwn.find(*a_key); it != g_engineOwn.end()) {
					return it->second;
				}
			}
			return staff_item::OwnEnchantment(a_held);
		}

		void RememberEngineOwn(const staff_item::Held& a_held, const core::StaffKey& a_key, RE::MagicItem* a_slot)
		{
			if (spell_copy::IsCopy(a_slot) || g_engineOwn.contains(a_key)) {
				return;
			}
			g_engineOwn[a_key] = a_slot;
			auto* guess = staff_item::OwnEnchantment(a_held);
			selftest::Report("T-OWN", guess == a_slot,
				std::format("{}: engine slot {:08X}, record/item guess {:08X}", KeyText(a_key), a_slot ? a_slot->GetFormID() : 0u, guess ? guess->GetFormID() : 0u));
		}

		// Ends a binding on the held item: name back, record gone.
		void Unbind(const staff_item::Held& a_held, const core::StaffKey& a_key, const char* a_why)
		{
			const auto* binding = g_table.Find(a_key);
			const char* name = binding ? staff_item::RestoreName(a_held, binding->name) : "no record";
			g_table.Remove(a_key);
			logger::info("binding {} ended ({}): {}", KeyText(a_key), a_why, name);
		}

		struct Pick
		{
			Hand                          hand{ Hand::kRight };
			RE::FormID                    spell{ 0 };
			RE::FormID                    staff{ 0 };
			std::optional<core::StaffKey> key;  // when the item already had an id at pick time
			std::uint32_t                 weaponSeq{ 0 };
		};

		void ApplyPick(const Pick& a_pick)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			if (g_off.load() != Off::kNo) {
				return;
			}
			const auto held = InHand(player, a_pick.hand);
			// With an id at pick time the key check below proves it is the same item; the counter only guards a staff
			// that had none (another staff of the same kind may be in the hand now).
			const bool sameItemUnproven = !a_pick.key && a_pick.weaponSeq != g_weaponSeq.load();
			if (sameItemUnproven || !held || held->staff->GetFormID() != a_pick.staff) {
				logger::info("pick {:08X} dropped: the {}-hand weapon changed after it", a_pick.spell, Name(a_pick.hand));
				Notify(messages::Text(messages::Msg::kPickDropped));
				return;
			}
			auto*      spell = RE::TESForm::LookupByID<RE::SpellItem>(a_pick.spell);
			const auto verdict = core::ClassifyForStaff(spell_copy::TraitsOf(spell, player), true);
			if (verdict != core::BindVerdict::kAllowed) {
				logger::info("pick {:08X} dropped: {}", a_pick.spell, core::Describe(verdict));
				return;
			}
			if (held->count != 1) {
				Notify(messages::Text(messages::Msg::kSingleOnly, std::to_string(held->count)));
				return;
			}
			if (staff_item::NameLocked(*held)) {
				Notify(messages::Text(messages::Msg::kNameLocked));
				return;
			}
			// With an id at pick time, compare first and give no id to another item (Opus 2026-11-19).
			const auto key = staff_item::KeyOf(player, *held, !a_pick.key.has_value());
			if (!key || (a_pick.key && *a_pick.key != *key)) {
				logger::warn("pick {:08X} dropped: the item id is {} (pick time {})", a_pick.spell, key ? KeyText(*key) : "missing",
					a_pick.key ? KeyText(*a_pick.key) : "none");
				Notify(messages::Text(messages::Msg::kPickDropped));  // the equip was taken by the menu hook: say so
				return;
			}
			if (!g_table.Find(*key) && g_table.Full()) {
				logger::warn("pick {:08X} dropped: {} bindings, the most a save keeps", a_pick.spell, g_table.Size());
				Notify(messages::Text(messages::Msg::kTableFull, std::to_string(g_table.Size())));
				return;
			}
			if (auto* caster = Caster(player, a_pick.hand); caster && caster->state.underlying() != 0) {
				caster->InterruptCast(false);  // a menu pick cuts the running cast first (SCS Equipment.cpp:52-56)
			}
			if (!g_table.Find(*key)) {
				TakeLostName(*held);  // a name left from a binding lost with the item's old id is not kept as the original
			}
			const auto            current = staff_item::DisplayName(*held);
			const auto            before = g_table.Find(*key) ? std::optional<core::NameState>{ g_table.Find(*key)->name } : std::nullopt;
			const std::string     spellName{ core::SpellNameForStaff(staff_mark::CleanName(spell)) };
			const core::NameState fresh{ staff_item::HasOwnName(*held), current, core::ComposeName(current, spellName) };
			switch (g_table.Toggle(*key, spell->GetFormID(), fresh)) {
			case core::ToggleResult::kAssigned:
				{
					const bool fits = fresh.written.size() <= core::BindingTable::kMaxNameBytes;  // the co-save keeps names up to this length
					const bool written = fits && staff_item::WriteName(*held, fresh.written);
					if (!written) {
						g_table.Find(*key)->name.written.clear();  // nothing of ours to take off later
					}
					selftest::Report("T-NAME", written || !fits, std::format("{} '{}' (fits={})", KeyText(*key), fresh.written, fits));
					logger::info("binding {} <- {:08X} '{}' in the {} hand (name {})", KeyText(*key), spell->GetFormID(), spellName, Name(a_pick.hand), written ? "written" : "NOT written");
					Notify(messages::Text(messages::Msg::kNowCasts, current, spellName));
					break;
				}
			case core::ToggleResult::kChanged:
				{
					auto& binding = *g_table.Find(*key);
					binding.name = core::Rebase(binding.name, current);
					const auto name = core::ComposeName(binding.name.original, spellName);
					const bool fits = name.size() <= core::BindingTable::kMaxNameBytes;
					if (!fits) {
						staff_item::RestoreName(*held, binding.name);  // the old "[spell]" would otherwise stay with no record (Codex r2)
					}
					const bool written = fits && staff_item::WriteName(*held, name);
					binding.name.written = written ? name : std::string{};
					selftest::Report("T-NAME", written || !fits, std::format("{} '{}' (fits={})", KeyText(*key), name, fits));
					logger::info("binding {} changed to {:08X} '{}' (name {})", KeyText(*key), spell->GetFormID(), spellName, written ? "written" : "NOT written");
					Notify(messages::Text(messages::Msg::kNowCasts, binding.name.original, spellName));
					break;
				}
			case core::ToggleResult::kCleared:
				{
					const char* name = before ? staff_item::RestoreName(*held, *before) : "no record";
					logger::info("binding {} cleared by picking {:08X} again: {}", KeyText(*key), spell->GetFormID(), name);
					Notify(messages::Text(messages::Msg::kOwnAgain, staff_item::DisplayName(*held)));
					break;
				}
			}
			SyncHand(player, a_pick.hand);  // the slot follows in the same frame
		}

		// The picks made while the magic menu is open (the game is paused, so none applies until it closes). Only the
		// latest per hand counts: the 2026-11-17 test clicked Healing five times in one menu, waiting for something to
		// happen, and the picks toggled assign/clear/assign/clear/assign. One click or five, the staff now ends up the
		// same. A pick for the other hand is kept beside it (Astra 2026-11-20: right A then left B kept only B).
		std::mutex                         g_pickLock;
		std::array<std::optional<Pick>, 2> g_pendingPick;

		void ApplyPendingPick()
		{
			std::array<std::optional<Pick>, 2> picks;
			{
				const std::scoped_lock lock{ g_pickLock };
				picks.swap(g_pendingPick);
			}
			for (const auto& pick : picks) {
				if (pick) {
					ApplyPick(*pick);
				}
			}
		}

		// The pick applies when the menu closes (the game is paused until then), but the star should move at once (user
		// 2026-11-18: "the marker moved only after closing and reopening the menu"). Run as a UI task, after the menu's
		// equip callback returned, so the item list is not rebuilt under the callback that is using it. The marker goes
		// where the pick will put the binding: off when the same spell is picked again, else on the picked spell.
		void PreviewMark()
		{
			const auto thread = REX::W32::GetCurrentThreadId();
			if (thread != frame_hook::MainThreadId()) {
				logger::info("marker preview skipped: UI task on thread {}, not the main thread", thread);
				return;
			}
			auto*      player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			// Both hands' pending picks at once: a second pick in the same menu must not hide the first one's marker
			// (Astra 2026-11-20). A hand without a pending pick keeps what it shows.
			std::array<std::optional<Pick>, 2> pending;
			{
				const std::scoped_lock lock{ g_pickLock };
				pending = g_pendingPick;
			}
			auto shown = g_activeSpell;
			for (const auto hand : kHands) {
				const auto& pick = pending[Index(hand)];
				const auto  held = pick ? InHand(player, hand) : std::nullopt;
				if (!held) {
					continue;
				}
				const auto  key = staff_item::KeyOf(player, *held, false);
				const auto* binding = key ? g_table.Find(*key) : nullptr;
				shown[Index(hand)] = binding && binding->spell == pick->spell ? nullptr : RE::TESForm::LookupByID<RE::SpellItem>(pick->spell);
			}
			staff_mark::OnFrame(shown[Index(Hand::kRight)], shown[Index(Hand::kLeft)]);
			auto* ui = RE::UI::GetSingleton();
			auto  menu = ui ? ui->GetMenu<RE::MagicMenu>() : nullptr;
			// Only the game's own MagicMenu object has this layout; a mod that registered another class under the same
			// menu name would be misread, so its vtable must be the game's.
			static const REL::Relocation<std::uintptr_t> magicMenuVtable{ RE::VTABLE_MagicMenu[0] };
			const bool gameMenu = menu && *reinterpret_cast<const std::uintptr_t*>(menu.get()) == magicMenuVtable.address();
			auto*      list = gameMenu ? menu->GetRuntimeData().itemList : nullptr;
			if (list) {
				list->Update();  // the engine rebuilds the entries from the spells, names included
			}
			logger::info("marker preview: right {:08X}, left {:08X}, magic menu list {}", shown[0] ? shown[0]->GetFormID() : 0u, shown[1] ? shown[1]->GetFormID() : 0u,
				list ? "refreshed" : "not open (another menu draws it)");
		}

		// Chained after SCS's own interception of the magic menu's spell equip (SCS MenuEquipHook.cpp).
		void MenuEquipThunk(RE::ActorEquipManager* a_manager, RE::Actor* a_actor, RE::SpellItem* a_spell, RE::BGSEquipSlot** a_slot)
		{
			auto*      player = RE::PlayerCharacter::GetSingleton();
			const auto slotId = a_slot && *a_slot ? (*a_slot)->GetFormID() : 0;
			const bool handSlot = slotId == kRightHandSlot || slotId == kLeftHandSlot;
			const Hand hand = slotId == kLeftHandSlot ? Hand::kLeft : Hand::kRight;
			const bool taking = g_off.load() == Off::kNo;  // the uninstall option takes no new picks
			const auto held = taking && player && a_actor == player && a_spell && handSlot ? InHand(player, hand) : std::nullopt;
			if (!held) {
				g_previousMenuEquip(a_manager, a_actor, a_spell, a_slot);
				return;
			}
			const auto verdict = core::ClassifyForStaff(spell_copy::TraitsOf(a_spell, player), true);
			if (verdict != core::BindVerdict::kAllowed) {
				// Equipped to the hand as usual (the staff goes down), with the original slot pointer (design 3).
				Notify(messages::Text(messages::Msg::kCannotBind, std::string(staff_mark::CleanName(a_spell)), messages::Raw(messages::ReasonFor(verdict))));
				g_previousMenuEquip(a_manager, a_actor, a_spell, a_slot);
				return;
			}
			// Checked here too, not only when the pick is applied: otherwise the equip was swallowed and the spell went
			// neither on the staff nor into the hand (Carbon 2026-11-17).
			if (held->count != 1 || staff_item::NameLocked(*held)) {
				Notify(held->count != 1 ? messages::Text(messages::Msg::kSingleOnly, std::to_string(held->count)) : messages::Text(messages::Msg::kNameLocked));
				g_previousMenuEquip(a_manager, a_actor, a_spell, a_slot);
				return;
			}
			// The staff stays in the hand: the equip is consumed and applied on the main thread when the menu closes.
			bool post = false;
			{
				const std::scoped_lock lock{ g_pickLock };
				auto& pending = g_pendingPick[Index(hand)];
				post = !g_pendingPick[0] && !g_pendingPick[1];  // one queued task applies both hands' picks
				if (pending) {
					logger::info("pick {:08X} replaces the earlier {} hand pick {:08X} made in the same menu", a_spell->GetFormID(), Name(hand), pending->spell);
				}
				pending = Pick{ hand, a_spell->GetFormID(), held->staff->GetFormID(), staff_item::KeyOf(player, *held, false), g_weaponSeq.load() };
			}
			if (post) {
				main_queue::Post(ApplyPendingPick);
			}
			if (auto* tasks = SKSE::GetTaskInterface()) {
				tasks->AddUITask([] { PreviewMark(); });
			}
		}

		struct UniqueIdSink final : RE::BSTEventSink<RE::TESUniqueIDChangeEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESUniqueIDChangeEvent* a_event, RE::BSTEventSource<RE::TESUniqueIDChangeEvent>*) override
			{
				if (!a_event || a_event->objectID == 0) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto* weapon = RE::TESForm::LookupByID<RE::TESObjectWEAP>(a_event->objectID);
				if (weapon && weapon->IsStaff()) {  // a log line per staff event, so T-UNIQ can be judged even when nothing matched
					logger::info("unique id event for staff {:08X}: ({:08X},{}) -> ({:08X},{})", a_event->objectID, a_event->oldBaseID, a_event->oldUniqueID,
						a_event->newBaseID, a_event->newUniqueID);
				}
				const std::scoped_lock lock{ g_moveLock };
				g_pendingMoves.emplace_back(core::StaffKey{ a_event->objectID, a_event->oldBaseID, a_event->oldUniqueID },
					core::StaffKey{ a_event->objectID, a_event->newBaseID, a_event->newUniqueID });
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		UniqueIdSink g_uniqueIdSink;

		struct EquipSink final : RE::BSTEventSink<RE::TESEquipEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				// Staves only: taking a dagger out of the left hand (equipping a left-hand spell over it) must not drop a
				// pick made for the right-hand staff (Carbon 2026-11-19). The event names no hand.
				if (a_event && a_event->actor && a_event->actor->IsPlayerRef() && staff_item::IsTarget(RE::TESForm::LookupByID<RE::TESObjectWEAP>(a_event->baseObject))) {
					g_weaponSeq.fetch_add(1);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		EquipSink g_equipSink;

		// [Uninstall] bRestoreNames: every bound staff the player carries gets its name back and loses its binding.
		// Bindings of staves elsewhere (chests, followers) stay in the table: their names cannot be reached from here.
		void RunUninstall()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			std::size_t restored = 0;
			staff_item::ForEachWithId(player, [&](const staff_item::Held& a_held, const core::StaffKey& a_key) {
				if (g_table.Find(a_key)) {
					Unbind(a_held, a_key, "uninstall option");
					++restored;
				}
			});
			logger::warn("uninstall option: {} bindings ended in the inventory, {} left (staves not carried)", restored, g_table.Size());
			Notify(messages::Text(messages::Msg::kUninstallDone, std::to_string(restored)));
		}
	}

	void InstallMenuHook()
	{
		// SCS installs its required menu hook at kDataLoaded and refuses to run if the call no longer goes to the engine
		// (in-game test 2026-11-07: "REQUIRED MENU HOOK FAILED" when we chained first). So this runs once, later.
		static bool installed = false;
		if (installed) {
			return;
		}
		installed = true;
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESUniqueIDChangeEvent>(&g_uniqueIdSink);
			events->AddEventSink<RE::TESEquipEvent>(&g_equipSink);
		}
		const REL::Relocation<std::uintptr_t> parent{ REL::RelocationID(37952, 38908) };
		const auto                            site = parent.address() + 0xD7;  // the call SCS hooks (SCS MenuEquipHook.cpp:42-43)
		if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8) {
			logger::critical("menu equip call site {:X} is not a CALL (another mod patched it?); spells cannot be put on staves, saved bindings are kept but not applied",
				site);
			g_off = Off::kMenuHook;
			return;
		}
		g_menuHooked = true;
		std::int32_t displacement = 0;
		std::memcpy(&displacement, reinterpret_cast<const void*>(site + 1), sizeof(displacement));
		g_previousMenuEquip = reinterpret_cast<MenuEquipFn>(site + 5 + static_cast<std::intptr_t>(displacement));
		SKSE::GetTrampoline().write_call<5>(site, MenuEquipThunk);
		logger::info("Hook installed: magic menu spell equip call (chained after {:X})", reinterpret_cast<std::uintptr_t>(g_previousMenuEquip));
	}

	// One hand (declared in staff_binding.h for ApplyPick): put the bound copy in its slot, or the staff's own enchantment back. Whether a target staff is there.
	bool SyncHand(RE::PlayerCharacter* a_player, Hand a_hand)
	{
		g_activeSpell[Index(a_hand)] = nullptr;
		const auto held = InHand(a_player, a_hand);
		if (g_saving.load()) {
			return held.has_value();
		}
		auto* const slot = hand_slot::Current(a_player, a_hand);
		const bool  idle = CasterIdle(a_player, a_hand);
		if (!held) {
			g_dormant[Index(a_hand)].reset();  // moved to the other hand or put away: its notice may come again there (Opus)
			if (spell_copy::IsCopy(slot) && idle) {
				hand_slot::Write(a_player, a_hand, nullptr);  // the staff left the hand without the engine rewriting the slot
			}
			return false;
		}
		const auto key = staff_item::KeyOf(a_player, *held, false);
		auto*      binding = key && g_off.load() == Off::kNo ? g_table.Find(*key) : nullptr;  // off: the restore-own path below runs
		RE::SpellItem* spell = binding && binding->spell ? RE::TESForm::LookupByID<RE::SpellItem>(binding->spell) : nullptr;
		const auto     state = core::DecideHeld(binding != nullptr, binding ? binding->spell : 0, spell != nullptr, spell && spell->IsDynamicForm(),
            spell && a_player->HasSpell(spell));
		if (state == core::HeldBinding::kUnbind) {
			const auto staffName = staff_item::DisplayName(*held);
			Unbind(*held, *key, binding->spell ? "its spell no longer exists" : "its spell's plugin is gone");
			Notify(messages::Text(messages::Msg::kSpellGone, staffName));
			binding = nullptr;
			spell = nullptr;
		} else if (state == core::HeldBinding::kNone) {
			TakeLostName(*held);
		}
		// Not known right now (Heart of Magic takes spells away while it swaps or re-locks a learning target): the
		// binding stays, the staff casts its own enchantment, and the spell comes back by itself (user 2026-11-17).
		const bool dormant = state == core::HeldBinding::kDormant;
		auto&      dormantKey = g_dormant[Index(a_hand)];
		if (dormant != (dormantKey == key)) {
			if (dormant) {
				dormantKey = key;
				logger::info("binding {} dormant: spell {:08X} is not known now", KeyText(*key), binding->spell);
				Notify(messages::Text(messages::Msg::kDormant, staff_item::DisplayName(*held)));
			} else if (dormantKey && dormantKey == key) {
				dormantKey.reset();
				logger::info("binding {} active again", KeyText(*key));
			}
		}
		RE::MagicItem* wanted = nullptr;
		bool           restoreOwn = false;
		if (binding && !dormant) {
			g_activeSpell[Index(a_hand)] = spell;
			wanted = spell_copy::Get(spell);  // null only when the form factory failed: the slot is left alone then
		} else if (spell_copy::IsCopy(slot)) {
			wanted = OwnFor(*held, key);  // null for a staff without an enchantment: the slot is emptied
			restoreOwn = true;
		} else {
			return true;  // an unbound staff: the engine's slot stays as it is
		}
		if ((wanted || restoreOwn) && slot != wanted && idle) {
			if (!restoreOwn) {
				RememberEngineOwn(*held, *key, slot);
			}
			hand_slot::Write(a_player, a_hand, wanted);
			if (!restoreOwn && g_rebindPending) {
				g_rebindPending = false;
				selftest::Report("T-SAVE-REBIND", hand_slot::Current(a_player, a_hand) == wanted,
					std::format("{} in the {} hand got copy {:08X} of {:08X} after the load", KeyText(*key), Name(a_hand), wanted->GetFormID(), spell->GetFormID()));
			}
		}
		return true;
	}

	HandsHeld OnFrame(RE::PlayerCharacter* a_player)
	{
		ApplyPendingMoves();
		HandsHeld held{};
		for (const auto hand : kHands) {
			held[Index(hand)] = SyncHand(a_player, hand);
		}
		return held;
	}

	void OnSave(std::uint32_t a_mainThread)
	{
		const auto thread = REX::W32::GetCurrentThreadId();
		selftest::Report("T-THREAD", a_mainThread == 0 || thread == a_mainThread, std::format("kSaveGame on thread {} (frame hook {})", thread, a_mainThread));
		if (thread == a_mainThread) {
			// Picked in the magic menu, then saved from the system menu without going back to the game: the queue has not
			// run (Carbon 2026-11-17). Apply it now so the save has the binding and the name.
			ApplyPendingPick();
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		// kSaveGame arrives before the save is written, synchronously, so this cannot wait for the queue. Raw writes:
		// each copy keeps its preload reference until it gets its slot back (design 5, exception for the save). One task
		// puts both hands back (Astra 2026-11-20: one copy may sit in both slots, each with its own reference).
		struct Swap
		{
			RE::MagicItem* copy{ nullptr };
			RE::MagicItem* own{ nullptr };
		};
		std::array<Swap, 2> swaps{};
		bool                swapped = false;
		for (const auto hand : kHands) {
			auto* copy = hand_slot::Current(player, hand);
			if (!spell_copy::IsCopy(copy)) {
				continue;
			}
			const auto held = InHand(player, hand);
			const auto key = held ? staff_item::KeyOf(player, *held, false) : std::nullopt;
			auto*      own = held ? OwnFor(*held, key) : nullptr;
			hand_slot::WriteRaw(player, hand, own);
			swaps[Index(hand)] = { copy, own };
			swapped = true;
			logger::info("save: {} hand slot {:08X} -> {:08X} while the save is written", Name(hand), copy->GetFormID(), own ? own->GetFormID() : 0u);
		}
		if (!swapped) {
			return;
		}
		g_saving = true;
		main_queue::Post([swaps] {
			g_saving = false;
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			for (const auto hand : kHands) {
				const auto& [copy, own] = swaps[Index(hand)];
				if (!copy) {
					continue;
				}
				if (hand_slot::Current(player, hand) == own) {
					hand_slot::WriteRaw(player, hand, copy);  // the copy still holds this slot's preload reference
					logger::info("save: {} hand slot back to {:08X}", Name(hand), copy->GetFormID());
				} else {
					// The engine set the slot itself: its setter preloaded the new item and released own, which had no
					// reference of ours left (our first copy write released it). Give own one back and drop the copy's.
					hand_slot::MoveReference(copy, own);
					logger::info("save: the {} hand slot changed during the save; copy {:08X} released, own {:08X} re-preloaded", Name(hand), copy->GetFormID(),
						own ? own->GetFormID() : 0u);
				}
			}
			OnFrame(player);  // the bindings may have changed meanwhile: the usual path fixes both slots
		});
	}

	void OnReset()
	{
		// A load right after a save drops the queued put-back (main_queue generation): its copy keeps one preload
		// reference, which only keeps its models loaded.
		g_saving = false;
		g_dormant = {};
		g_engineOwn.clear();
		{
			const std::scoped_lock lock{ g_moveLock };
			g_pendingMoves.clear();  // id changes of the game being left (Codex r2); the loaded save's come after this
		}
		g_rebindPending = false;
		g_lostNames.clear();  // names of the game being left
		g_weaponSeq.fetch_add(1);
		const std::scoped_lock lock{ g_pickLock };
		g_pendingPick = {};  // its queued task was dropped with the old generation: the next pick must post again
	}

	void OnLoaded(std::size_t a_bindings)
	{
		g_rebindPending = a_bindings > 0;
	}

	void OnPostLoad()
	{
		if (!g_menuHooked) {
			static bool shown = false;  // once a game run: the hook is not retried
			if (!shown) {
				shown = true;
				RE::DebugMessageBox(messages::Text(messages::Msg::kMenuHookFailed).c_str());
			}
		}
		if (Settings::Get().restoreNames) {  // works with the menu hook missing too: it only needs the inventory
			g_off = Off::kUninstall;
			main_queue::Post(RunUninstall);
		}
	}

	core::BindingTable& TableForSave()
	{
		ApplyPendingMoves();
		return g_table;
	}

	void RunUninstallNow()
	{
		g_off = Off::kUninstall;
		RunUninstall();
	}

	bool Applying()
	{
		return g_off.load() == Off::kNo;
	}

	core::BindingTable& Table()
	{
		return g_table;
	}

	RE::SpellItem* ActiveSpell(Hand a_hand)
	{
		return g_activeSpell[Index(a_hand)];
	}
}
