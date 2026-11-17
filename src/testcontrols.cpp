#include "PCH.h"

#include "testcontrols.h"

#include "conversion.h"
#include "detour_helper.h"
#include "keywords.h"
#include "pool.h"
#include "selftest.h"
#include "settings.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include <cctype>
#include <cmath>
#include <intrin.h>
#include <mutex>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace scssc::testcontrols
{
	namespace
	{
		constexpr std::uint32_t kKeyBind = 0x43;       // F9
		constexpr std::uint32_t kKeyAttackRight = 0x44; // F10
		constexpr std::uint32_t kKeyAttackLeft = 0x57;  // F11
		constexpr std::uint32_t kMouseLeftButton = 0;   // engine id of the left mouse button (right-hand attack)
		constexpr std::uint32_t kMouseRightButton = 1;  // right mouse button (left-hand attack)
		constexpr RE::FormID    kRightHandSlot = 0x13F42;
		constexpr RE::FormID    kLeftHandSlot = 0x13F43;

		using MenuEquipFn = void (*)(RE::ActorEquipManager*, RE::Actor*, RE::SpellItem*, RE::BGSEquipSlot**);

		struct Hold
		{
			bool  active{ false };
			bool  left{ false };
			float elapsed{ 0.0f };
			float duration{ 0.0f };
		};
		Hold g_hold;

		bool Enabled()
		{
			const auto& settings = Settings::Get();
			return settings.selfTest && settings.testControls;
		}

		void Push(bool a_left, float a_value, float a_heldSecs)
		{
			if (auto* queue = RE::BSInputEventQueue::GetSingleton()) {
				const RE::BSFixedString userEvent = a_left ? "Left Attack/Block" : "Right Attack/Block";
				queue->AddButtonEvent(RE::INPUT_DEVICE::kMouse, static_cast<std::int32_t>(a_left ? kMouseRightButton : kMouseLeftButton),
					a_value, a_heldSecs, userEvent);
			}
		}

		void StartAttack(bool a_left)
		{
			if (g_hold.active) {
				logger::info("TEST attack ignored: a hold is already running");
				return;
			}
			g_hold = { true, a_left, 0.0f, static_cast<float>(Settings::Get().testHoldMs) / 1000.0f };
			Push(a_left, 1.0f, 0.0f);
			logger::info("TEST attack {} pressed (hold {:.2f}s)", a_left ? "L" : "R", g_hold.duration);
		}

		// The address the menu's equip call jumps to: SCS's interception thunk when SCS hooked it.
		MenuEquipFn ScsMenuEquipTarget()
		{
			const REL::Relocation<std::uintptr_t> parent{ REL::RelocationID(37952, 38908) };
			const auto site = parent.address() + 0xD7;  // the call SCS hooks (MenuEquipHook.cpp:42-43)
			if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8) {
				return nullptr;
			}
			std::int32_t displacement = 0;
			std::memcpy(&displacement, reinterpret_cast<const void*>(site + 1), sizeof(displacement));
			const auto target = site + 5 + static_cast<std::intptr_t>(displacement);
			const auto base = REL::Module::get().base();
			const auto text = REL::Module::get().segment(REL::Segment::textx);
			const bool insideGame = target >= base + text.offset() && target < base + text.offset() + text.size();
			// Inside the game code = nobody hooked the call; calling it would equip the spell to the hand for real.
			return insideGame ? nullptr : reinterpret_cast<MenuEquipFn>(target);
		}

		void BindChargeSpell(bool a_left)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* weapon = player ? player->GetEquippedObject(a_left) : nullptr;
			auto* staff = weapon ? weapon->As<RE::TESObjectWEAP>() : nullptr;
			auto* spell = staff ? pool::FindSpell(keywords::OriginalEnchantment(staff->GetFormID())) : nullptr;
			auto* data = RE::TESDataHandler::GetSingleton();
			auto* slot = data ? data->LookupForm<RE::BGSEquipSlot>(a_left ? kLeftHandSlot : kRightHandSlot, "Skyrim.esm") : nullptr;
			const auto target = ScsMenuEquipTarget();
			if (!spell || !slot || !target || !player->HasSpell(spell)) {
				logger::warn("TEST bind {} refused: staff={} spell={} learned={} scsMenuHook={}", a_left ? "L" : "R", staff != nullptr,
					spell != nullptr, spell && player->HasSpell(spell), target != nullptr);
				return;
			}
			RE::BGSEquipSlot* slotPtr = slot;
			target(RE::ActorEquipManager::GetSingleton(), player, spell, &slotPtr);
			logger::info("TEST bind {} spell {:08X} sent through SCS's menu equip path; hand now holds {:08X}", a_left ? "L" : "R",
				spell->GetFormID(), player->GetEquippedObject(a_left) ? player->GetEquippedObject(a_left)->GetFormID() : 0u);
		}

		// Development spike (2026-11-08) for the standalone design: copy a spell into a per-item enchantment, the way
		// Immersive Staves does (QTR-Modding/ProjectStaff, GPL-3). Its staves have no record enchantment; this checks a
		// vanilla staff that has one, with the record left alone. F9 on a staff we do not convert walks through the steps.
		// T-ART 2x2 (2026-11-16): one-shot vs concentration x the staff's own tip art (Fireball01HandEffects 0001B211)
		// vs another (FrostBoltHandFX01.nif, 000334B9 and 00020BF5). Cast types read with houseCARL; in this load order
		// NoviceBoltSpell-Mysticism.esp turns Sparks, Flames and Frostbite into fire-and-forget, so creature spells stand in.
		constexpr RE::FormID kSpikeFirebolt = 0x12FD0;     // fire-and-forget, art 0001B211
		constexpr RE::FormID kSpikeIceSpike = 0x2B96C;     // fire-and-forget, art 000334B9
		constexpr RE::FormID kSpikeFlames = 0x7E881;       // crAtronachFlameFlames: concentration, art 0001B211
		constexpr RE::FormID kSpikeFrostStream = 0x6445C;  // crAtronachCloakIceDmg: concentration, aimed, art 00020BF5

		enum class StepSource
		{
			kSpellCopy,         // a runtime enchantment copied from a spell
			kGameEnchantment,   // the Skyrim.esm enchantment itself
			kEnchantmentClone,  // a runtime clone of that enchantment, same data
		};

		struct SpikeStep
		{
			RE::FormID  form;  // the spell, or the Skyrim.esm enchantment
			bool        payMagicka;
			StepSource  source;
			bool        matchRecord;  // point the staff record's enchantment at the slot's while held
			const char* label;
		};
		constexpr RE::FormID kSpikeOakflesh = 0x5AD5C;  // self buff with a duration: leaves an ActiveEffect that references the copy

		// The charge-paid and item-attached steps of the 2026-11-10 run are done (T-C*, Case 296) and are left out: the
		// standalone design puts nothing on the item, and a save during an item step was not a valid T-SAVE sample.
		// T-ART hypothesis H-D (2026-11-16): the tip art is never loaded when the slot holds a form made at runtime. The
		// third test showed a matched record is not enough (after a sheathe and draw the art request stalled again).
		// Steps 1-4 are a 2x2 on one enchantment, Staff of Ice Storm (art 000334B9), paid from the staff's charge: the
		// game's own record or a runtime clone with the same data, the staff record matched or not. Only the form's
		// origin and the record match change. Step 5 is the restart test.
		constexpr RE::FormID kSpikeIceStormEnchantment = 0x29B5A;  // StaffEnchIceStorm, fire-and-forget, art FrostBoltHandFX01

		// Final layout (2026-11-16, after reading CheckAttachCastingArt 34185 in the 1.6.1170 code): the engine only starts
		// a hand's casting art once the slot item is preloaded (11304); the engine preloads what it puts in a hand slot
		// (11303) and releases what it takes out (11306), which this spike did not. Every step is now the shipping
		// combination: a runtime copy of a spell, paid from magicka, the staff record left alone. Consecutive steps change
		// the art model, so a live swap (no sheathe) is visible at each F9. The 2x2 above is kept for reference only.
		constexpr std::array kSpikeSteps{
			SpikeStep{ kSpikeIceSpike, true, StepSource::kSpellCopy, false, "Ice Spike copy (frost tip), magicka" },
			SpikeStep{ kSpikeFirebolt, true, StepSource::kSpellCopy, false, "Firebolt copy (fire tip), magicka" },
			SpikeStep{ kSpikeFrostStream, true, StepSource::kSpellCopy, false, "Frost stream copy (frost tip, concentration), magicka" },
			SpikeStep{ kSpikeOakflesh, true, StepSource::kSpellCopy, false, "Oakflesh copy (shield tip): restart test" },
		};
		[[maybe_unused]] constexpr std::array kSpike2x2{
			SpikeStep{ kSpikeIceStormEnchantment, false, StepSource::kGameEnchantment, false, "Ice Storm, game's own enchantment, record not matched" },
			SpikeStep{ kSpikeIceStormEnchantment, false, StepSource::kEnchantmentClone, true, "Ice Storm, clone made in game, record matched" },
		};

		struct SpikeState
		{
			int                  step{ -1 };
			RE::TESObjectWEAP*   staff{ nullptr };
			RE::ExtraDataList*   list{ nullptr };  // the exact item under test
			RE::MagicItem*       ownSlot{ nullptr };
			RE::EnchantmentItem* ownRecord{ nullptr };  // the staff record's enchantment before the spike swapped it
			RE::EnchantmentItem* installedRecord{ nullptr };  // what the spike last wrote into the record (nullptr: own)
			RE::FormID           uidBase{ 0 };       // ExtraUniqueID of the exact item under test: another staff of the same
			std::uint16_t        uid{ 0 };           // kind is a different item, and inventory lists can be rebuilt
			bool                 afterSave{ false };  // the save put the slot back: the next cast follows a fresh slot change
			bool                 sawDetach{ false };    // the art was off at some frame since this step began
			bool                 freshAttach{ false };  // ... and was then attached again: a new attach under this step
			bool                 swapPassed{ false };   // T-ART-SWAP saw this step's art attached after the F9
			std::string          originalName;
			bool                 hadNameData{ false };
			const RE::MagicItem* lastSpell{ nullptr };
			std::uint32_t        lastState{ 0 };
		};
		SpikeState                  g_spike;
		std::atomic<RE::MagicItem*> g_magickaEnchantment{ nullptr };
		std::atomic<RE::MagicItem*> g_stepEnchantment{ nullptr };    // the copy under test, paid either way
		std::atomic<RE::MagicItem*> g_recordEnchantment{ nullptr };  // the staff record's own enchantment

		using ResourceFn = RE::ActorValue (*)(RE::MagicItem*, RE::MagicSystem::CastingSource);
		ResourceFn g_originalResource = nullptr;

		// One line per distinct caller in each shot. The list is short and cleared every shot, so a linear scan is enough.
		struct LookupCall
		{
			std::uintptr_t callerOffset{ 0 };
			RE::FormID     item{ 0 };
			std::int32_t   source{ 0 };
			std::int32_t   engineValue{ 0 };
			std::int32_t   returnedValue{ 0 };
			std::uint32_t  calls{ 0 };
		};
		constexpr std::size_t                 kMaxLookupCalls = 24;
		std::mutex                            g_lookupLock;
		std::array<LookupCall, kMaxLookupCalls> g_lookupCalls{};
		std::size_t                           g_lookupCallCount = 0;
		std::uint32_t                         g_lookupCallsDropped = 0;

		void RecordLookup(const LookupCall& a_call)
		{
			const std::scoped_lock lock{ g_lookupLock };
			for (std::size_t i = 0; i < g_lookupCallCount; ++i) {
				auto& known = g_lookupCalls[i];
				if (known.callerOffset == a_call.callerOffset && known.item == a_call.item && known.source == a_call.source &&
					known.engineValue == a_call.engineValue && known.returnedValue == a_call.returnedValue) {
					++known.calls;
					return;
				}
			}
			if (g_lookupCallCount == g_lookupCalls.size()) {
				++g_lookupCallsDropped;
				return;
			}
			g_lookupCalls[g_lookupCallCount] = a_call;
			g_lookupCalls[g_lookupCallCount].calls = 1;
			++g_lookupCallCount;
		}

		// The lookup has no actor argument: an NPC using the same record enchantment lands in the same table, so run the
		// test with no such NPC nearby.
		void FlushLookups(bool a_log, bool a_skipEmpty = false)
		{
			const std::scoped_lock lock{ g_lookupLock };
			if (a_log && !(a_skipEmpty && g_lookupCallCount == 0)) {
				logger::info("SPIKE lookup calls since the last report: {} distinct, {} dropped", g_lookupCallCount, g_lookupCallsDropped);
				for (std::size_t i = 0; i < g_lookupCallCount; ++i) {
					const auto& call = g_lookupCalls[i];
					logger::info("SPIKE lookup caller=+{:X} item={:08X} source={} engine={} returned={} calls={}", call.callerOffset, call.item, call.source,
						call.engineValue, call.returnedValue, call.calls);
				}
			}
			g_lookupCallCount = 0;
			g_lookupCallsDropped = 0;
		}

		// The game's own lookup at +6C72E5 (AE 1.6.1170) asks for the item enchantment's resource. With the record
		// pointed at the copy it got Magicka, and the staff never finished charging (2026-11-16 21:23; the likely cause,
		// what that function does with the value is not read); before that it asked about the record's own enchantment
		// and got the item charge. That caller keeps the engine's answer.
		constexpr std::uintptr_t kItemChargeCaller1170 = 0x6C72E5;

		RE::ActorValue ResourceThunk(RE::MagicItem* a_item, RE::MagicSystem::CastingSource a_source)
		{
			const auto engineValue = g_originalResource(a_item, a_source);
			const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - REL::Module::get().base();
			static const bool is1170 = REL::Module::get().version() == REL::Version{ 1, 6, 1170, 0 };
			const bool itemChargeCaller = is1170 && caller == kItemChargeCaller1170;
			const auto returnedValue = a_item && a_item == g_magickaEnchantment.load() && !itemChargeCaller ? RE::ActorValue::kMagicka : engineValue;
			if (a_item && (a_item == g_stepEnchantment.load() || a_item == g_recordEnchantment.load())) {
				RecordLookup({ caller, a_item->GetFormID(),
					static_cast<std::int32_t>(a_source), static_cast<std::int32_t>(engineValue), static_cast<std::int32_t>(returnedValue) });
			}
			return returnedValue;
		}

		// Hooks the engine's cost-resource lookup itself (the place Immersive Staves Tweaks hooks), so a caller outside
		// the three sites used by Immersive Staves is redirected too, and the shot log names every caller.
		void InstallResourceHook()
		{
			const REL::Relocation<std::uintptr_t> lookup{ RELOCATION_ID(33817, 34609) };
			g_originalResource = reinterpret_cast<ResourceFn>(lookup.address());
			if (const auto err = detour::Attach(reinterpret_cast<void**>(&g_originalResource), reinterpret_cast<void*>(&ResourceThunk)); err != 0) {
				g_originalResource = nullptr;
				logger::warn("SPIKE cost hook off: could not hook the resource lookup at {:X} (Detours error {})", lookup.address(), err);
				return;
			}
			logger::info("SPIKE cost hook installed on the resource lookup at {:X}", lookup.address());
		}

		RE::ExtraDataList* WornRightList(RE::PlayerCharacter* a_player, const RE::TESObjectWEAP* a_staff)
		{
			auto* changes = a_player->GetInventoryChanges();
			if (!changes || !changes->entryList) {
				return nullptr;
			}
			for (auto* entry : *changes->entryList) {
				if (!entry || entry->object != a_staff || !entry->extraLists) {
					continue;
				}
				for (auto* list : *entry->extraLists) {
					if (list && list->HasType(RE::ExtraDataType::kWorn)) {
						return list;
					}
				}
			}
			return nullptr;
		}

		bool IsSpikeItem(const RE::ExtraDataList* a_list)
		{
			const auto* id = a_list ? a_list->GetByType<RE::ExtraUniqueID>() : nullptr;
			return id && g_spike.uidBase != 0 && id->baseID == g_spike.uidBase && id->uniqueID == g_spike.uid;
		}

		// The exact item under test, found again by its unique ID (never by a remembered list pointer). nullptr when the
		// player no longer has it.
		RE::ExtraDataList* FindSpikeList(RE::PlayerCharacter* a_player)
		{
			auto* changes = a_player->GetInventoryChanges();
			if (!changes || !changes->entryList || !g_spike.staff) {
				return nullptr;
			}
			for (auto* entry : *changes->entryList) {
				if (!entry || entry->object != g_spike.staff || !entry->extraLists) {
					continue;
				}
				for (auto* list : *entry->extraLists) {
					if (IsSpikeItem(list)) {
						return list;
					}
				}
			}
			return nullptr;
		}

		bool HoldsSpikeItem(RE::PlayerCharacter* a_player)
		{
			if (a_player->GetEquippedObject(false) != g_spike.staff) {
				return false;
			}
			const auto* list = FindSpikeList(a_player);
			return list && list->HasType(RE::ExtraDataType::kWorn);
		}

		// T-SAVE fix under test (2026-11-16). The save keeps an active effect as a reference to its source form, but a
		// runtime copy's effect list is not saved, so after a restart the copy's effects were dropped (2026-11-10:
		// 7 of 7 lost). Effects are re-pointed at the original spell, which the game data always has, at the moment
		// they are applied. Copies are never freed in this spike, so the map only grows; the real mod needs a lifetime
		// policy for copies.
		std::mutex                                               g_copyLock;
		std::unordered_map<const RE::MagicItem*, RE::SpellItem*> g_copyToOriginal;
		std::atomic<std::uint32_t>                               g_redirects{ 0 };
		std::atomic<std::uint32_t>                               g_redirectMisses{ 0 };
		std::uint32_t                                            g_redirectsReported = 0;
		std::uint32_t                                            g_redirectMissesReported = 0;

		RE::SpellItem* OriginalOf(const RE::MagicItem* a_item)
		{
			if (!a_item || !a_item->IsDynamicForm()) {
				return nullptr;
			}
			const std::scoped_lock lock{ g_copyLock };
			const auto            it = g_copyToOriginal.find(a_item);
			return it != g_copyToOriginal.end() ? it->second : nullptr;
		}

		// Effects the hook re-pointed, so T-SAVE-COLD tracks only those and not a hand cast of the same spell. Pointers
		// are only compared, never read; the set is cleared when a save loads.
		std::unordered_set<const RE::ActiveEffect*> g_redirectedEffects;

		// Returns the original spell's effect the data now points at, or nullptr when nothing was changed.
		RE::Effect* RedirectToOriginal(RE::MagicTarget::AddTargetData& a_data)
		{
			auto* original = OriginalOf(a_data.magicItem);
			if (!original) {
				return nullptr;
			}
			// A concentration effect ends with the cast and is not kept across a restart anyway (T-KIND).
			if (original->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
				return nullptr;
			}
			const auto& copyEffects = a_data.magicItem->effects;
			for (std::uint32_t i = 0; i < copyEffects.size(); ++i) {
				if (copyEffects[i] != a_data.effect) {
					continue;
				}
				auto* target = i < original->effects.size() ? original->effects[i] : nullptr;
				if (!target) {
					break;
				}
				logger::info("SPIKE redirect {:08X}[{}] -> {:08X}[{}] mgef={:08X} applied magnitude={:.2f} (spell's {:.2f}) duration={} (spell's {})",
					a_data.magicItem->GetFormID(), i, original->GetFormID(), i, target->baseEffect ? target->baseEffect->GetFormID() : 0u, a_data.magnitude,
					target->effectItem.magnitude, a_data.effect ? a_data.effect->effectItem.duration : 0u, target->effectItem.duration);
				a_data.magicItem = original;
				a_data.effect = target;
				return target;
			}
			++g_redirectMisses;
			logger::warn("SPIKE redirect missed: copy {:08X} applied an effect that is not in its own list", a_data.magicItem->GetFormID());
			return nullptr;
		}

		// The target's effects of (spell, effect) before the engine adds one; the one that is new afterwards is the
		// redirected effect, not an older hand-cast one. A refresh of an existing effect adds nothing and is not marked.
		std::vector<const RE::ActiveEffect*> MatchingEffects(RE::MagicTarget* a_target, const RE::MagicItem* a_spell, const RE::Effect* a_effect)
		{
			std::vector<const RE::ActiveEffect*> result;
			if (auto* effects = a_target ? a_target->GetActiveEffectList() : nullptr) {
				for (const auto* effect : *effects) {
					if (effect && effect->spell == a_spell && effect->effect == a_effect) {
						result.push_back(effect);
					}
				}
			}
			return result;
		}

		void RememberRedirected(RE::MagicTarget* a_target, const RE::MagicItem* a_spell, const RE::Effect* a_effect,
			const std::vector<const RE::ActiveEffect*>& a_before)
		{
			for (const auto* effect : MatchingEffects(a_target, a_spell, a_effect)) {
				if (std::ranges::find(a_before, effect) == a_before.end()) {
					const std::scoped_lock lock{ g_copyLock };
					g_redirectedEffects.insert(effect);
					return;
				}
			}
		}

		template <std::size_t Id>
		struct AddTargetHook
		{
			static bool Thunk(RE::MagicTarget* a_this, RE::MagicTarget::AddTargetData& a_data)
			{
				// The caller's struct is put back afterwards in case the engine reuses it for the next effect.
				auto* const item = a_data.magicItem;
				auto* const effect = a_data.effect;
				auto* const target = RedirectToOriginal(a_data);
				const auto  before = target ? MatchingEffects(a_this, a_data.magicItem, target) : std::vector<const RE::ActiveEffect*>{};
				const bool  result = original(a_this, a_data);
				if (target && result) {
					++g_redirects;
					RememberRedirected(a_this, a_data.magicItem, target, before);
				}
				a_data.magicItem = item;
				a_data.effect = effect;
				return result;
			}
			static inline REL::Relocation<decltype(&Thunk)> original;
		};

		// MagicTarget::AddTarget is vfunc 1 of the MagicTarget base, the 5th vtable of Character and PlayerCharacter
		// (MagicTarget.h:115, Actor.h:145; the same slot TACTICDataCollection hooks).
		void InstallAddTargetHooks()
		{
			REL::Relocation<std::uintptr_t> character{ RE::VTABLE_Character[4] };
			AddTargetHook<0>::original = character.write_vfunc(0x1, AddTargetHook<0>::Thunk);
			REL::Relocation<std::uintptr_t> player{ RE::VTABLE_PlayerCharacter[4] };
			AddTargetHook<1>::original = player.write_vfunc(0x1, AddTargetHook<1>::Thunk);
			logger::info("SPIKE AddTarget hooks installed (Character, PlayerCharacter)");
		}

		std::size_t CountDynamicEffects(RE::PlayerCharacter* a_player)
		{
			std::size_t count = 0;
			auto*       target = a_player ? a_player->AsMagicTarget() : nullptr;
			if (auto* effects = target ? target->GetActiveEffectList() : nullptr) {
				for (const auto* effect : *effects) {
					if (effect && OriginalOf(effect->spell)) {  // only this spike's copies, not player-made enchantments or potions
						++count;
					}
				}
			}
			return count;
		}

		RE::EnchantmentItem* CopySpellToEnchantment(RE::SpellItem* a_spell)
		{
			auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::EnchantmentItem>();
			auto* enchantment = factory ? factory->Create() : nullptr;
			if (!enchantment) {
				return nullptr;
			}
			// No AddChange(1): it only put an empty change record for the copy into the save (T-SAVE hypothesis H2).
			enchantment->hostileCount = a_spell->hostileCount;
			enchantment->avEffectSetting = a_spell->avEffectSetting;
			for (const auto* effect : a_spell->effects) {
				auto* copy = new RE::Effect();
				copy->effectItem = effect->effectItem;
				copy->baseEffect = effect->baseEffect;
				copy->cost = effect->cost;
				// Conditions are left out on purpose: TESCondition assignment shares the list and its destructor frees it
				// (Immersive Staves assigns it anyway). The real mod needs a deep copy; this spike only asks whether the cast works.
				enchantment->effects.push_back(copy);
			}
			auto& data = enchantment->data;
			data.spellType = RE::MagicSystem::SpellType::kStaffEnchantment;
			data.castingType = a_spell->GetCastingType();
			data.delivery = a_spell->GetDelivery();
			data.chargeTime = a_spell->GetChargeTime();
			data.costOverride = a_spell->data.costOverride;
			if (a_spell->data.flags.all(RE::SpellItem::SpellFlag::kCostOverride)) {
				data.flags.set(RE::EnchantmentItem::EnchantmentFlag::kCostOverride);
			}
			// RedirectToOriginal pairs effects by index, which holds because every effect is copied in order above.
			const std::scoped_lock lock{ g_copyLock };
			g_copyToOriginal[enchantment] = a_spell;
			return enchantment;
		}

		// Runtime clones of a game enchantment (T-ART H-D): the same data as the original, only made at runtime. Not in
		// g_copyToOriginal (nothing to redirect), so tracked here for the save handling. Never freed in this spike.
		std::unordered_set<const RE::MagicItem*> g_enchantmentClones;

		bool IsSpikeCopy(const RE::MagicItem* a_item)
		{
			if (OriginalOf(a_item)) {
				return true;
			}
			const std::scoped_lock lock{ g_copyLock };
			return a_item && g_enchantmentClones.contains(a_item);
		}

		RE::EnchantmentItem* CloneEnchantment(const RE::EnchantmentItem* a_source)
		{
			auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::EnchantmentItem>();
			auto* clone = factory ? factory->Create() : nullptr;
			if (!clone) {
				return nullptr;
			}
			clone->hostileCount = a_source->hostileCount;
			clone->avEffectSetting = a_source->avEffectSetting;
			for (const auto* effect : a_source->effects) {
				auto* copy = new RE::Effect();
				copy->effectItem = effect->effectItem;
				copy->baseEffect = effect->baseEffect;
				copy->cost = effect->cost;  // conditions left out, as in CopySpellToEnchantment
				clone->effects.push_back(copy);
			}
			clone->data = a_source->data;
			const std::scoped_lock lock{ g_copyLock };
			g_enchantmentClones.insert(clone);
			return clone;
		}

		void SpikeNotify(const std::string& a_text)
		{
			logger::info("SPIKE {}", a_text);
			RE::SendHUDMessage::ShowHUDMessage(("Staff test: " + a_text).c_str());
		}

		float Value(RE::PlayerCharacter* a_player, RE::ActorValue a_value)
		{
			auto* values = a_player->AsActorValueOwner();
			return values ? values->GetActorValue(a_value) : std::numeric_limits<float>::quiet_NaN();
		}

		RE::ActorMagicCaster* RightHandCaster(RE::PlayerCharacter* a_player)
		{
			auto* caster = a_player->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand);
			return caster ? static_cast<RE::ActorMagicCaster*>(caster) : nullptr;
		}

		// What the right-hand caster has of its tip art: the art waiting to be attached (castingArt), whether one is
		// attached (kCastingArtAttached), its 3D (castingArtData.attachedArt) and a running model load (cloneTask).
		struct ArtState
		{
			RE::FormID             pending{ 0 };
			bool                   attached{ false };
			bool                   cloneRequested{ false };
			bool                   cloneTask{ false };
			const RE::NiNode*      node{ nullptr };
			std::string            nodeName;
			std::string            childNames;  // the attached node is a wrapper ('MagicRight' on 2026-11-16); the art sits below
			bool                   nodeHasParent{ false };
			const RE::NiNode*      root{ nullptr };  // castingArtData.current3DRoot
			std::uint32_t          attachPoint{ 0 };
			std::uint32_t          state{ 0 };

			bool operator==(const ArtState& a_other) const
			{
				return pending == a_other.pending && attached == a_other.attached && cloneRequested == a_other.cloneRequested &&
				       cloneTask == a_other.cloneTask && node == a_other.node && childNames == a_other.childNames && nodeHasParent == a_other.nodeHasParent &&
				       root == a_other.root && attachPoint == a_other.attachPoint && state == a_other.state;
			}
		};

		constexpr int kNodeSearchDepth = 4;

		void CollectChildNames(const RE::NiNode* a_node, int a_depth, std::string& a_out)
		{
			if (!a_node || a_depth <= 0) {
				return;
			}
			for (const auto& child : a_node->GetChildren()) {
				if (!child) {
					continue;
				}
				a_out += a_out.empty() ? "" : ",";
				a_out += child->name.c_str();
				CollectChildNames(child->AsNode(), a_depth - 1, a_out);
			}
		}

		bool SameName(std::string_view a_left, std::string_view a_right);

		bool HasNamedNode(const RE::NiAVObject* a_object, std::string_view a_name, int a_depth)
		{
			if (!a_object || a_name.empty()) {
				return false;
			}
			if (SameName(a_object->name.c_str(), a_name)) {
				return true;
			}
			const auto* node = const_cast<RE::NiAVObject*>(a_object)->AsNode();  // AsNode is a non-const cast in CommonLib; nothing is written
			if (!node || a_depth <= 0) {
				return false;
			}
			for (const auto& child : node->GetChildren()) {
				if (HasNamedNode(child.get(), a_name, a_depth - 1)) {
					return true;
				}
			}
			return false;
		}

		ArtState ReadArtState(const RE::ActorMagicCaster* a_caster)
		{
			ArtState result;
			result.pending = a_caster->castingArt ? a_caster->castingArt->GetFormID() : 0;
			result.attached = a_caster->flags.all(RE::ActorMagicCaster::Flags::kCastingArtAttached);
			result.cloneRequested = a_caster->flags.all(RE::ActorMagicCaster::Flags::kStartCloneTask);
			result.cloneTask = a_caster->cloneTask.get() != nullptr;
			result.node = a_caster->castingArtData.attachedArt.get();
			result.nodeName = result.node ? result.node->name.c_str() : "";
			CollectChildNames(result.node, kNodeSearchDepth, result.childNames);
			result.root = a_caster->castingArtData.current3DRoot.get();
			result.attachPoint = a_caster->castingArtData.attachPoint;
			// On the actor's skeleton: the parent chain must end at its first- or third-person 3D root, not at a tree
			// that was taken off it.
			if (result.node && a_caster->actor) {
				const auto* first = a_caster->actor->Get3D(true);
				const auto* third = a_caster->actor->Get3D(false);
				for (const RE::NiAVObject* up = result.node->parent; up; up = up->parent) {  // any ancestor: the root may have a parent itself
					if (up == first || up == third) {
						result.nodeHasParent = true;
						break;
					}
				}
			}
			result.state = a_caster->state.underlying();
			return result;
		}

		std::string DescribeArtState(const ArtState& a_state)
		{
			return std::format("state={} pending={:08X} attached={} cloneRequested={} cloneTask={} node='{}' children='{}' onSkeleton={} root={} attachPoint={}",
				a_state.state, a_state.pending, a_state.attached, a_state.cloneRequested, a_state.cloneTask, a_state.nodeName, a_state.childNames,
				a_state.nodeHasParent, static_cast<const void*>(a_state.root), a_state.attachPoint);
		}

		RE::BGSArtObject* TipArtOf(const RE::MagicItem* a_item)
		{
			const auto* main = a_item ? a_item->GetCostliestEffectItem() : nullptr;
			return main && main->baseEffect ? main->baseEffect->data.castingArt : nullptr;
		}

		// The rest of the caster around a deselect/select, so a first crash or side effect shows in the log (the log
		// flushes every line). Whether these calls read currentSpell (null while idle) or the hand slot is not known.
		std::string DescribeCaster(RE::PlayerCharacter* a_player, const RE::ActorMagicCaster* a_caster)
		{
			const auto* state = a_player->AsActorState();
			// The engine's attach step takes the light from selectedSpells[source]->GetAVEffect() (Black Flame notes,
			// CasterArt.cpp): logged for the slot item, since a runtime form may differ from a game one here.
			const auto* slotItem = a_player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand];
			const auto* avField = slotItem ? slotItem->avEffectSetting : nullptr;
			const auto* avEffect = slotItem ? slotItem->GetAVEffect() : nullptr;
			return std::format("currentSpell={:08X} flags={:X} magicNode={} light={} sounds={} enchController={} weaponDrawn={} slotAvField={:08X} slotAvEffect={:08X}",
				a_caster->currentSpell ? a_caster->currentSpell->GetFormID() : 0u, a_caster->flags.underlying(), static_cast<const void*>(a_caster->magicNode),
				static_cast<const void*>(a_caster->light.get()), a_caster->sounds.size(), static_cast<const void*>(a_caster->weaponEnchantmentController),
				state && state->IsWeaponDrawn(), avField ? avField->GetFormID() : 0u, avEffect ? avEffect->GetFormID() : 0u);
		}

		// MagicItem preload and release (AE ids 11303 / 11306, read in the decrypted 1.6.1170 code on 2026-11-16:
		// 11303 bumps preloadCount (+0x80) and queues the item's models into preloadedItem (+0x88); 11306 drops the count
		// and frees the queued file at zero). Only on 1.6.1170, the build whose code was read; elsewhere nothing is called.
		using PreloadFn = void (*)(RE::MagicItem*);
		using ReleasePreloadFn = void (*)(RE::MagicItem*, bool);
		PreloadFn        g_preload = nullptr;
		ReleasePreloadFn g_releasePreload = nullptr;

		bool PreloadInstalled() { return g_preload && g_releasePreload; }

		// SKSE's AddTask runs on job worker threads during normal play (measured by HoM Black Flame, MainThreadQueue.h:
		// 409 of 415 task bodies off the main thread). Every engine change here (hand slot, preload, art) is posted to this
		// queue instead and run at the top of OnFrame, which the Main::Update frame call runs on the main thread. While the
		// game is paused OnFrame is not called, so posted work waits for the unpause. A global lock: the queue is tiny.
		std::atomic<std::uint32_t>         g_resetGeneration{ 0 };  // ResetSpike (load, new game) counts up
		std::mutex                         g_mainQueueLock;
		std::vector<std::function<void()>> g_mainQueue;

		void PostMain(std::function<void()> a_task)
		{
			const std::scoped_lock lock{ g_mainQueueLock };
			g_mainQueue.push_back(std::move(a_task));
		}

		void DrainMain()
		{
			std::vector<std::function<void()>> tasks;
			{
				const std::scoped_lock lock{ g_mainQueueLock };
				tasks.swap(g_mainQueue);
			}
			for (auto& task : tasks) {
				task();
			}
		}

		using DetachArtFn = void (*)(RE::ActorMagicCaster*, bool);
		using SetCastingArtFn = void (*)(RE::ActorMagicCaster*, RE::BGSArtObject*);
		DetachArtFn     g_detachArt = nullptr;
		SetCastingArtFn g_setCastingArt = nullptr;

		void InstallPreload()
		{
			if (REL::Module::get().version() != REL::Version{ 1, 6, 1170, 0 }) {
				logger::warn("SPIKE preload: not 1.6.1170, the hand slot items are not preloaded (the tip art will not load)");
				return;
			}
			g_preload = reinterpret_cast<PreloadFn>(REL::ID(11303).address());
			g_releasePreload = reinterpret_cast<ReleasePreloadFn>(REL::ID(11306).address());
			logger::info("SPIKE preload: 11303 at {:X}, 11306 at {:X}", reinterpret_cast<std::uintptr_t>(g_preload), reinterpret_cast<std::uintptr_t>(g_releasePreload));
			// The Black Flame route (HoM BlackFlame HandArtRefresh.cpp, shown working in game there): the engine's own unready
			// and ready steps of 38784, DetachCastingArt 34152 with true, then SetCastingArt 34150.
			g_detachArt = reinterpret_cast<DetachArtFn>(REL::ID(34152).address());
			g_setCastingArt = reinterpret_cast<SetCastingArtFn>(REL::ID(34150).address());
			logger::info("SPIKE art fallback: 34152 at {:X}, 34150 at {:X}", reinterpret_cast<std::uintptr_t>(g_detachArt), reinterpret_cast<std::uintptr_t>(g_setCastingArt));
		}

		// T-ART hypothesis H-B (2026-11-16 test; Carbon and GPT both rank it first): one frame after the hand slot changes,
		// the engine drops the attached tip art and asks for the new one (kStartCloneTask), but never starts loading it
		// while the slot's enchantment differs from the held staff's enchantment. The 11-07 SCS build, which worked, kept
		// the two the same by swapping the staff record's enchantment while held, because the engine's item lookup prefers
		// the record over a per-item enchantment (11-06 T-C2a). So the record is pointed at the slot's enchantment before
		// the slot is written. The record is shared by every staff of the kind while this runs (an NPC holding one would
		// cast the copy); it is put back at the finish, on loads and while a save is written. Deselect/SelectSpellImpl
		// were dropped: the 11-16 test showed they change nothing. Only called while the caster is idle.
		// Every write of the right-hand slot goes through here, so each item in the slot holds exactly one preload
		// reference. What the engine does when it changes a hand slot (38770 for the power slot: 11303 on the new item,
		// 11306 on the old one, then the write): the new item's models are preloaded, which CheckAttachCastingArt waits for
		// (11304: preloadedItem done). Without it the art request stalled forever (11-16 runs). Returns whether it preloaded.
		bool SetHandSlot(RE::PlayerCharacter* a_player, RE::MagicItem* a_item)
		{
			auto&       slot = a_player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand];
			auto* const old = slot;
			const bool  preload = PreloadInstalled() && old != a_item;
			if (preload && a_item) {
				g_preload(a_item);
			}
			if (preload && old) {
				g_releasePreload(old, true);
			}
			slot = a_item;
			return preload;
		}

		void RebindStaff(RE::PlayerCharacter* a_player, RE::TESObjectWEAP* a_staff, RE::MagicItem* a_newSlot, bool a_matchRecord, const char* a_why)
		{
			auto& slot = a_player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand];
			auto* caster = RightHandCaster(a_player);
			auto* wanted = TipArtOf(a_newSlot);
			auto* enchantment = a_matchRecord && a_newSlot ? a_newSlot->As<RE::EnchantmentItem>() : nullptr;
			auto* record = enchantment ? enchantment : g_spike.ownRecord;
			if (caster) {
				logger::info("SPIKE art [{}] wanted={:08X} '{}' slot {:08X} -> {:08X} record {:08X} -> {:08X} before: {} | {}", a_why, wanted ? wanted->GetFormID() : 0u,
					wanted ? wanted->GetModel() : "", slot ? slot->GetFormID() : 0u, a_newSlot ? a_newSlot->GetFormID() : 0u,
					a_staff->formEnchanting ? a_staff->formEnchanting->GetFormID() : 0u, record ? record->GetFormID() : 0u, DescribeArtState(ReadArtState(caster)),
					DescribeCaster(a_player, caster));
			}
			if (record) {
				a_staff->formEnchanting = record;  // matched to the slot, or back to its own when this step does not match
				g_spike.installedRecord = enchantment;
			}
			const bool preload = SetHandSlot(a_player, a_newSlot);
			if (caster) {
				logger::info("SPIKE art [{}] after (preload {}): {} | {} newPreloadCount={}", a_why, preload ? "done" : "skipped", DescribeArtState(ReadArtState(caster)),
					DescribeCaster(a_player, caster), a_newSlot ? a_newSlot->preloadCount : 0u);
			}
		}

		void RestoreStaffRecord(const char* a_why)
		{
			if (g_spike.staff && g_spike.ownRecord && g_spike.installedRecord && g_spike.staff->formEnchanting != g_spike.ownRecord) {
				if (g_spike.staff->formEnchanting != g_spike.installedRecord) {
					// Someone else changed the record since the spike swapped it: their value is left alone.
					logger::warn("SPIKE record [{}]: holds {:08X}, not what the spike put there; left as is", a_why,
						g_spike.staff->formEnchanting ? g_spike.staff->formEnchanting->GetFormID() : 0u);
					return;
				}
				logger::info("SPIKE record [{}]: {:08X} -> {:08X}", a_why, g_spike.staff->formEnchanting ? g_spike.staff->formEnchanting->GetFormID() : 0u,
					g_spike.ownRecord->GetFormID());
				g_spike.staff->formEnchanting = g_spike.ownRecord;
			}
		}

		// The engine renames the attached art's root to its attach node ('MagicRight', 2026-11-16 21:23 log), so the art
		// is recognised by a node inside it that only that model has. Hand-made table for the test arts only (houseCARL
		// nif_inspect 2026-11-16 on the installed winners); any other art is not judged (SKIP).
		struct ArtMarker
		{
			std::string_view model;   // file stem of BGSArtObject::GetModel()
			std::string_view marker;  // a node only this model has
		};
		constexpr std::array kArtMarkers{
			ArtMarker{ "Fireball01HandEffects", "pFireballCore04" },
			ArtMarker{ "FrostBoltHandFX01", "pSmallSnow03" },
			ArtMarker{ "ShieldSpellHand01", "BigCube01" },
		};

		bool SameName(std::string_view a_left, std::string_view a_right);

		std::string ExpectedNodeName(const RE::BGSArtObject* a_art)
		{
			const char* model = a_art ? a_art->GetModel() : nullptr;
			if (!model || !*model) {
				return {};
			}
			const std::string stem = std::filesystem::path(model).stem().string();
			for (const auto& entry : kArtMarkers) {
				if (SameName(stem, entry.model)) {
					return std::string{ entry.marker };
				}
			}
			logger::info("SPIKE art: no marker node known for '{}'; casts with it are not judged", model);
			return {};
		}

		bool SameName(std::string_view a_left, std::string_view a_right)
		{
			return a_left.size() == a_right.size() && std::ranges::equal(a_left, a_right, [](char a_l, char a_r) {
				return std::tolower(static_cast<unsigned char>(a_l)) == std::tolower(static_cast<unsigned char>(a_r));
			});
		}

		// Frame-by-frame record of the tip art while the spike runs: a line whenever it changes, and per cast whether the
		// wanted art stayed attached on the skeleton from the start of the charge to the release. The cast is pinned to the
		// step and copy it started with, so a later F9 or another spell cannot take its result.
		struct ArtWatch
		{
			ArtState             last;
			bool                 known{ false };
			float                clock{ 0.0f };
			bool                 casting{ false };
			bool                 valid{ false };
			bool                 control{ false };
			bool                 afterSave{ false };
			bool                 afterRedraw{ false };
			int                  step{ -1 };
			const RE::MagicItem* copy{ nullptr };
			std::string          wantedNode;
			float                castStart{ 0.0f };
			std::uint32_t        castFrames{ 0 };
			std::uint32_t        framesWanted{ 0 };
			std::uint32_t        framesOther{ 0 };
			std::uint32_t        framesOnSkeleton{ 0 };
			std::uint32_t        detaches{ 0 };
			float                firstWantedMs{ -1.0f };
			std::string          otherNode;
		};
		ArtWatch        g_artWatch;

		// T-ART-SWAP: after an F9 changes the hand slot (staff drawn, caster idle), the new art must be attached within a
		// few seconds without a sheathe or a cast. Judged from the frame watch.
		struct SwapCheck
		{
			bool        active{ false };
			int         step{ -1 };
			float       start{ 0.0f };
			std::string marker;
			bool        fallbackTried{ false };  // the preload route timed out and the Black Flame route was queued
			bool        fallbackRan{ false };    // its engine calls were made (judged only from then on)
		};
		SwapCheck       g_swapCheck;

		const char* SwapId()
		{
			return g_swapCheck.fallbackTried ? "T-ART-FALLBACK" : "T-ART-SWAP";
		}
		constexpr float kSwapTimeoutSecs = 3.0f;

		// T-PAY: a staff cast of a magicka-paid copy must lower magicka and leave the staff's charge alone.
		struct PaySample
		{
			bool  active{ false };
			float magicka{ 0.0f };
			float charge{ 0.0f };
			float cost{ 0.0f };
			float maxMagickaDrop{ 0.0f };
			float maxChargeChange{ 0.0f };
			bool  released{ false };
			bool  otherHandCast{ false };  // the left hand cast during the sample: its magicka use would count
			bool  concentration{ false };
			float peakMagicka{ 0.0f };     // highest magicka seen so far: regeneration before the payment raises it
			float streamSecs{ 0.0f };      // time spent streaming (state 6), for a concentration copy
			float lastClock{ 0.0f };
		};
		PaySample       g_paySample;
		constexpr float kArtGraceSecs = 0.15f;  // a model load may take a few frames after the charge starts

		// MagicCaster::State as logged on 2026-11-10: one shot 1 -> 2 -> 3 -> 4 -> 0, concentration 1 -> 2 -> 4 -> 6 -> 0.
		bool IsTipState(std::uint32_t a_state)
		{
			using State = RE::MagicCaster::State;
			return a_state == static_cast<std::uint32_t>(State::kUnk02) || a_state == static_cast<std::uint32_t>(State::kReady) ||
			       a_state == static_cast<std::uint32_t>(State::kCasting);
		}

		void StartCastWatch(const RE::MagicCaster* a_caster, std::string_view a_idle)
		{
			auto& watch = g_artWatch;
			watch.casting = true;
			watch.step = g_spike.step;
			watch.control = watch.step < 0;
			bool nativeControl = true;
			if (watch.control) {
				// The staff's own enchantment in the hand slot: the expected art comes from it the same way. Only a native
				// cast counts: slot, cast and the held staff's record must all be the same enchantment.
				const auto* actor = a_caster->GetCasterAsActor();
				watch.copy = actor ? actor->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand] : nullptr;
				const auto* weapon = actor ? actor->GetEquippedObject(false) : nullptr;
				const auto* staff = weapon ? weapon->As<RE::TESObjectWEAP>() : nullptr;
				nativeControl = staff && watch.copy && staff->formEnchanting == watch.copy && !OriginalOf(watch.copy);
			} else {
				watch.copy = g_stepEnchantment.load();
			}
			watch.valid = nativeControl && watch.copy && a_caster->currentSpell == watch.copy;
			watch.wantedNode = watch.copy ? ExpectedNodeName(TipArtOf(watch.copy)) : std::string{};
			watch.afterSave = g_spike.afterSave;
			// The control casts the staff's own art; a step counts once its art came off and was attached anew, or the live
			// swap passed.
			watch.afterRedraw = watch.control || g_spike.freshAttach || g_spike.swapPassed;
			if (!watch.control && watch.step >= 0 && kSpikeSteps[static_cast<std::size_t>(watch.step)].payMagicka) {
				auto* player = RE::PlayerCharacter::GetSingleton();
				g_paySample = {};
				g_paySample.active = true;
				g_paySample.magicka = Value(player, RE::ActorValue::kMagicka);
				g_paySample.charge = Value(player, RE::ActorValue::kRightItemCharge);
				g_paySample.cost = a_caster->currentSpellCost;
				g_paySample.peakMagicka = g_paySample.magicka;
				g_paySample.lastClock = watch.clock;
				g_paySample.concentration = watch.copy && watch.copy->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
			} else {
				g_paySample = {};
			}
			g_spike.afterSave = false;
			watch.castStart = watch.clock;
			watch.castFrames = watch.framesWanted = watch.framesOther = watch.framesOnSkeleton = watch.detaches = 0;
			watch.firstWantedMs = -1.0f;
			watch.otherNode.clear();
			logger::info("SPIKE art cast start: step={} copy={:08X} casting={:08X} valid={} wantedNode='{}' idle: {}", watch.step + 1,
				watch.copy ? watch.copy->GetFormID() : 0u, a_caster->currentSpell ? a_caster->currentSpell->GetFormID() : 0u, watch.valid, watch.wantedNode, a_idle);
		}

		void ReportPay(bool a_valid)
		{
			if (!g_paySample.active) {
				return;
			}
			const auto before = g_paySample;
			g_paySample = {};
			auto*      player = RE::PlayerCharacter::GetSingleton();
			const auto magicka = Value(player, RE::ActorValue::kMagicka);
			const auto charge = Value(player, RE::ActorValue::kRightItemCharge);
			const auto detail = std::format("magicka {:.2f} -> {:.2f} (deepest drop {:.2f}, cost {:.2f}), staff charge {:.2f} -> {:.2f} (largest change {:.2f}) released={}",
				before.magicka, magicka, before.maxMagickaDrop, before.cost, before.charge, charge, before.maxChargeChange, before.released);
			if (!a_valid) {
				selftest::Skip("T-PAY", detail + " (sample dropped)");
				return;
			}
			if (!before.released) {
				selftest::Skip("T-PAY", detail + " (the cast was cancelled before it was let go)");
				return;
			}
			constexpr float kMinJudgedCost = 1.0f;
			if (before.cost < kMinJudgedCost || RE::PlayerCharacter::IsGodMode()) {
				selftest::Skip("T-PAY", detail + " (cost under 1 or god mode: no payment to see)");
				return;
			}
			if (before.otherHandCast) {
				selftest::Skip("T-PAY", detail + " (the left hand cast during the sample)");
				return;
			}
			if (before.magicka < before.cost) {
				selftest::Skip("T-PAY", detail + " (magicka at the start was below the cost)");
				return;
			}
			// Magicka regenerates during a cast, so the deepest drop seen in any frame is judged against half the cost;
			// the charge must not move in any frame.
			constexpr float kChargeTolerance = 0.01f;
			constexpr float kMinCostShare = 0.5f;
			// A concentration copy pays its cost per second: half of what this stream's length should have cost. A stream
			// shorter than kMinStreamSecs is too short to tell a payment from frame noise.
			constexpr float kMinStreamSecs = 0.5f;
			if (before.concentration && before.streamSecs < kMinStreamSecs) {
				selftest::Skip("T-PAY", detail + std::format(" (stream of {:.2f} s is too short to judge)", before.streamSecs));
				return;
			}
			const float     expected = before.concentration ? before.cost * (std::min)(before.streamSecs, 1.0f) : before.cost;  // per-second or not, a stream of at least a second pays at least one cost
			const float     minDrop = (std::max)(kChargeTolerance, expected * kMinCostShare);
			const bool      paidMagicka = before.maxMagickaDrop >= minDrop;
			const bool      chargeKept = before.maxChargeChange <= kChargeTolerance && std::fabs(charge - before.charge) <= kChargeTolerance;
			// An empty staff cannot show a charge drop: the verdict then rests on magicka alone, and the line says so.
			selftest::Report("T-PAY", paidMagicka && chargeKept, before.charge <= kChargeTolerance ? detail + " (staff charge was empty: charge side not tested)" : detail);
		}

		void EndCastWatch()
		{
			auto& watch = g_artWatch;
			if (!watch.casting) {
				return;
			}
			watch.casting = false;
			const auto* label = watch.control ? "staff's own enchantment (control)" : kSpikeSteps[static_cast<std::size_t>(watch.step)].label;
			const auto  id = watch.control ? "T-ART-CONTROL" : "T-ART-ATTACH";
			const auto  detail = std::format(
				"step={} '{}' wanted='{}' framesWanted={}/{} framesOtherArt={} ('{}') framesOnSkeleton={} firstWantedMs={:.0f} detaches={} afterSave={}", watch.step + 1,
				label, watch.wantedNode, watch.framesWanted, watch.castFrames, watch.framesOther, watch.otherNode, watch.framesOnSkeleton, watch.firstWantedMs,
				watch.detaches, watch.afterSave);
			if (!watch.valid || watch.wantedNode.empty()) {
				selftest::Skip(id, detail + " (the hand did not cast the expected item, or its art has no model)");
			} else if (!watch.afterRedraw) {
				selftest::Skip(id, detail + " (the art has not come off since this step began: sheathe and draw the staff first)");
			} else if (watch.castFrames == 0) {
				selftest::Skip(id, detail + " (cast shorter than the grace time)");
			} else {
				selftest::Report(id, watch.framesWanted == watch.castFrames && watch.detaches == 0, detail);
			}
			ReportPay(watch.valid);
		}

		bool WeaponDrawn()
		{
			const auto* player = RE::PlayerCharacter::GetSingleton();
			const auto* state = player ? player->AsActorState() : nullptr;
			return state && state->GetWeaponState() == RE::WEAPON_STATE::kDrawn;
		}

		// A check or sample still open when the spike moves on, stops or resets is closed as SKIP with the reason, so
		// no step goes unjudged without a line in the log.
		void AbortChecks(const char* a_why)
		{
			if (g_swapCheck.active) {
				selftest::Skip(SwapId(), std::format("step={} marker '{}' ({})", g_swapCheck.step + 1, g_swapCheck.marker, a_why));
			}
			if (g_paySample.active) {
				selftest::Skip("T-PAY", std::format("cast sample open ({})", a_why));
			}
			g_swapCheck = {};
			g_paySample = {};
		}

		// a_old is the item the slot held before the F9. When its art has the same model marker as the new one, the
		// engine keeps the attached art (ledger 21:23:52), so a marker seen afterwards proves nothing: SKIP.
		void ArmSwapCheck(int a_step, const RE::MagicItem* a_old, const RE::MagicItem* a_item)
		{
			AbortChecks("another F9 before it was judged");
			g_spike.swapPassed = false;
			const auto marker = ExpectedNodeName(TipArtOf(a_item));
			const auto* oldArt = TipArtOf(a_old);
			// The slot was just written but the engine has not run yet: what is attached now is the old art (or, after two
			// quick F9s, an even older one), so the new marker must not already be there.
			const auto* caster = RightHandCaster(RE::PlayerCharacter::GetSingleton());
			const auto  attached = caster ? ReadArtState(caster) : ArtState{};
			if (!WeaponDrawn()) {
				selftest::Skip("T-ART-SWAP", std::format("step={} (the staff is not drawn: the live swap is judged only with it drawn)", a_step + 1));
			} else if (!marker.empty() && attached.attached && attached.node && HasNamedNode(attached.node, marker, kNodeSearchDepth)) {
				selftest::Skip("T-ART-SWAP", std::format("step={} (the art attached now already shows '{}': no swap to see)", a_step + 1, marker));
			} else if (oldArt == TipArtOf(a_item) || (!marker.empty() && marker == ExpectedNodeName(oldArt))) {
				selftest::Skip("T-ART-SWAP", std::format("step={} (the old slot item {:08X} shows the same art '{}': no swap to see)", a_step + 1,
					a_old ? a_old->GetFormID() : 0u, marker));
			} else {
				g_swapCheck = { true, a_step, g_artWatch.clock, marker };
			}
		}

		// Only an idle, drawn hand within the time limit can pass: a cast, a sheathe or the timeout ends the sample first.
		// The raw state of the slot item's preload job (engine 11304 wants it done: state 5 at +0xC), for the log only.
		std::string DescribePreload(const RE::MagicItem* a_item)
		{
			if (!a_item) {
				return "no item";
			}
			const auto* job = a_item->preloadedItem.get();
			const auto  state = job ? *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uintptr_t>(job) + 0xC) : 0u;
			return std::format("item {:08X} preloadCount={} job={} state=0x{:X}", a_item->GetFormID(), a_item->preloadCount, job ? "yes" : "none", state);
		}

		// The Black Flame route, queued to the main-thread task queue (as RebindStaff runs) and checked again there: the
		// same spike step, the staff drawn, the caster idle, the player's 3D present. Then 34152 (true) and 34150 with the
		// slot item's tip art; CheckAttachCastingArt attaches it over the next frames.
		void QueueArtFallback(int a_step)
		{
			PostMain([a_step] {
				auto*       player = RE::PlayerCharacter::GetSingleton();
				auto*       caster = player ? RightHandCaster(player) : nullptr;
				auto* const item = player ? player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand] : nullptr;
				auto* const art = TipArtOf(item);
				const char* refused = !g_swapCheck.active || !g_swapCheck.fallbackTried || g_swapCheck.step != a_step || g_spike.step != a_step ? "the check moved on" :
				                      !HoldsSpikeItem(player) || item != g_stepEnchantment.load()                                     ? "another staff or slot item" :
				                      !caster || !art                                                                                 ? "no caster or no art" :
				                      !WeaponDrawn()                                                                                  ? "not drawn" :
				                      caster->state.underlying() != 0                                                                 ? "caster not idle" :
				                      !player->Get3D()                                                                                ? "no 3D" :
				                                                                                                                        nullptr;
				if (refused) {
					logger::info("SPIKE art fallback for step {} not run: {}", a_step + 1, refused);
					if (g_swapCheck.active && g_swapCheck.fallbackTried && g_swapCheck.step == a_step) {
						g_swapCheck.active = false;
						selftest::Skip("T-ART-FALLBACK", std::format("step={} (not run: {})", a_step + 1, refused));
					}
					return;
				}
				logger::info("SPIKE art fallback for step {}: art {:08X} before: {} | {}", a_step + 1, art->GetFormID(), DescribeArtState(ReadArtState(caster)), DescribePreload(item));
				g_detachArt(caster, true);
				g_setCastingArt(caster, art);
				g_swapCheck.fallbackRan = true;
				g_swapCheck.start = g_artWatch.clock;  // the fallback's window starts when its calls ran
				logger::info("SPIKE art fallback for step {}: after: {}", a_step + 1, DescribeArtState(ReadArtState(caster)));
			});
		}

		// Only an idle, drawn hand within the time limit can pass: a cast, a sheathe or the timeout ends the sample first.
		// The first timeout is the preload route's FAIL (T-ART-SWAP); the Black Flame route then gets its own window
		// (T-ART-FALLBACK), so one test run answers both.
		void WatchSwap(const ArtState& a_now)
		{
			auto& check = g_swapCheck;
			if (!check.active) {
				return;
			}
			const auto elapsed = g_artWatch.clock - check.start;
			const auto detail = std::format("step={} marker '{}' after {:.0f} ms: {}", check.step + 1, check.marker, elapsed * 1000.0f, DescribeArtState(a_now));
			if (check.marker.empty()) {
				check.active = false;
				selftest::Skip(SwapId(), detail + " (no marker known for this art)");
			} else if (a_now.state != 0) {
				check.active = false;
				selftest::Skip(SwapId(), detail + " (a cast started before the art was judged)");
			} else if (!WeaponDrawn()) {
				check.active = false;
				selftest::Skip(SwapId(), detail + " (the staff was sheathed during the check)");
			} else if (check.fallbackTried && !check.fallbackRan) {
				// queued, not run yet: nothing is judged until its calls are made
			} else if (elapsed >= kSwapTimeoutSecs) {
				const auto* player = RE::PlayerCharacter::GetSingleton();
				const auto* item = player ? player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand] : nullptr;
				selftest::Report(SwapId(), false, detail + " | " + DescribePreload(item));
				if (!check.fallbackTried && g_detachArt && g_setCastingArt) {
					check.fallbackTried = true;
					check.start = g_artWatch.clock;
					QueueArtFallback(check.step);
				} else {
					check.active = false;
				}
			} else if (a_now.attached && a_now.node && HasNamedNode(a_now.node, check.marker, kNodeSearchDepth)) {
				check.active = false;
				g_spike.swapPassed = true;
				selftest::Report(SwapId(), true, std::format("step={} marker '{}' attached {:.0f} ms after the {}", check.step + 1, check.marker, elapsed * 1000.0f,
					check.fallbackTried ? "Black Flame route" : "F9"));
			}
		}

		// Per frame during a magicka-paid cast: the deepest magicka drop from the start (regeneration only adds, so a
		// deduction shows as a drop at some frame), the largest charge change, and whether the spell was let go.
		void WatchPay(const ArtState& a_now)
		{
			if (!g_paySample.active) {
				return;
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			// The drop is measured from the highest value seen so far, so regeneration before the payment cannot hide it.
			const float magicka = Value(player, RE::ActorValue::kMagicka);
			g_paySample.peakMagicka = (std::max)(g_paySample.peakMagicka, magicka);
			g_paySample.maxMagickaDrop = (std::max)(g_paySample.maxMagickaDrop, g_paySample.peakMagicka - magicka);
			const float step = g_artWatch.clock - g_paySample.lastClock;
			g_paySample.lastClock = g_artWatch.clock;
			if (a_now.state == static_cast<std::uint32_t>(RE::MagicCaster::State::kCasting) && step > 0.0f) {
				g_paySample.streamSecs += step;
			}
			g_paySample.maxChargeChange = (std::max)(g_paySample.maxChargeChange, std::fabs(Value(player, RE::ActorValue::kRightItemCharge) - g_paySample.charge));
			if (const auto* left = player->GetMagicCaster(RE::MagicSystem::CastingSource::kLeftHand); left && left->state.underlying() != 0) {
				g_paySample.otherHandCast = true;
			}
			using State = RE::MagicCaster::State;
			if (a_now.state == static_cast<std::uint32_t>(State::kUnk04) || a_now.state == static_cast<std::uint32_t>(State::kCasting)) {
				g_paySample.released = true;  // one shot let go (4), or a stream started (6)
			}
		}

		void WatchArt(RE::ActorMagicCaster* a_caster, float a_delta)
		{
			auto& watch = g_artWatch;  // its clock is advanced in OnFrame, before the queued work runs
			const auto now = ReadArtState(a_caster);
			if (g_spike.step >= 0) {
				WatchSwap(now);
				if (!now.attached || !now.node) {
					g_spike.sawDetach = true;
				} else if (g_spike.sawDetach && !g_spike.freshAttach) {
					g_spike.freshAttach = true;
					logger::info("SPIKE art: attached anew under step {} ({})", g_spike.step + 1, now.childNames.substr(0, 80));
				}
			}
			if (now.state != 0 && !watch.casting) {
				StartCastWatch(a_caster, watch.known ? DescribeArtState(watch.last) : std::string{ "unknown" });
			}
			if (now.state != 0) {
				if (watch.valid && a_caster->currentSpell && a_caster->currentSpell != watch.copy) {
					watch.valid = false;  // another spell took over mid-cast: this sample no longer measures the copy
					logger::info("SPIKE art cast now casts {:08X}, not copy {:08X}: sample dropped", a_caster->currentSpell->GetFormID(),
						watch.copy ? watch.copy->GetFormID() : 0u);
				}
				// The attached node is the engine's wrapper ('MagicRight'); the wanted art is looked for under it. Whether
				// the wrapper sits on the skeleton is logged, not judged: on 2026-11-16 the staff's own working art showed
				// onSkeleton=false while idle.
				const bool hasArt = now.attached && now.node;
				const bool wanted = hasArt && HasNamedNode(now.node, watch.wantedNode, kNodeSearchDepth);
				const auto sinceStart = watch.clock - watch.castStart;
				if (wanted && watch.firstWantedMs < 0.0f) {
					watch.firstWantedMs = sinceStart * 1000.0f;
				}
				if (hasArt && !wanted && watch.otherNode.empty()) {
					watch.otherNode = now.nodeName + "/" + now.childNames;
				}
				// Judged only while the tip should glow: charging and held (2, 3) for one shot, streaming (6) for
				// concentration. Starting (1) and the release (4) are only logged: the engine may drop the art on release.
				const bool judged = IsTipState(now.state);
				const bool hadArt = watch.known && IsTipState(watch.last.state) && watch.last.attached && watch.last.node;
				if (judged && sinceStart >= kArtGraceSecs && hadArt && !hasArt) {  // same grace as the frame count
					++watch.detaches;
				}
				if (judged && sinceStart >= kArtGraceSecs) {
					++watch.castFrames;
					watch.framesWanted += wanted ? 1 : 0;
					watch.framesOther += hasArt && !wanted ? 1 : 0;
					watch.framesOnSkeleton += hasArt && now.nodeHasParent ? 1 : 0;
				}
			}
			if (!watch.known || !(now == watch.last)) {
				logger::info("SPIKE art t={:.0f}ms {}", (watch.clock - watch.castStart) * 1000.0f, DescribeArtState(now));
			}
			WatchPay(now);  // also on the frame back to idle: a one-shot pays at the release
			if (now.state == 0 && watch.casting) {
				EndCastWatch();
			}
			watch.last = now;
			watch.known = true;
		}

		std::string DescribeUniqueId(RE::ExtraDataList* a_list)
		{
			const auto* id = a_list->GetByType<RE::ExtraUniqueID>();
			return id ? std::format("({:08X},{})", id->baseID, id->uniqueID) : "none";
		}

		// Every inventory stack of this staff: answers whether a worn list can hold more than one item (T-STACK) and
		// whether the unique ID survives moves and saves (T-UNIQ).
		void LogStaffIdentity(RE::PlayerCharacter* a_player, RE::TESObjectWEAP* a_staff, const char* a_when)
		{
			auto* changes = a_player->GetInventoryChanges();
			if (!changes || !changes->entryList) {
				return;
			}
			for (auto* entry : *changes->entryList) {
				if (!entry || entry->object != a_staff) {
					continue;
				}
				logger::info("SPIKE identity [{}] staff={:08X} countDelta={} lists={}", a_when, a_staff->GetFormID(), entry->countDelta,
					entry->extraLists ? std::distance(entry->extraLists->begin(), entry->extraLists->end()) : 0);
				if (!entry->extraLists) {
					continue;
				}
				for (auto* list : *entry->extraLists) {
					if (list) {
						const auto* name = list->GetDisplayName(a_staff);
						logger::info("SPIKE identity [{}]   worn={} count={} uid={} name='{}'", a_when, list->HasType(RE::ExtraDataType::kWorn), list->GetCount(),
							DescribeUniqueId(list), name ? name : "");
					}
				}
			}
		}

		void SetSpikeName(RE::ExtraDataList* a_list, const RE::TESObjectWEAP* a_staff, const RE::MagicItem* a_spell)
		{
			if (a_list->GetCount() != 1) {
				logger::warn("SPIKE name skipped for {:08X}: worn list holds {} items", a_staff->GetFormID(), a_list->GetCount());
				return;
			}
			a_list->SetOverrideName(std::format("{} [{}]", g_spike.originalName, a_spell->GetName()).c_str());
		}

		void RestoreSpikeName(RE::ExtraDataList* a_list)
		{
			if (g_spike.hadNameData) {
				a_list->SetOverrideName(g_spike.originalName.c_str());
			} else {
				a_list->RemoveByType(RE::ExtraDataType::kTextDisplayData);
			}
		}

		// Lets the engine assign the ID (its own function sends TESUniqueIDChangeEvent); the ID is read back and logged.
		void EnsureUniqueId(RE::PlayerCharacter* a_player, RE::TESObjectWEAP* a_staff, RE::ExtraDataList* a_list)
		{
			if (a_list->HasType(RE::ExtraDataType::kUniqueID)) {
				return;
			}
			if (auto* changes = a_player->GetInventoryChanges()) {
				changes->SetUniqueID(a_list, nullptr, a_staff);
			}
			logger::info("SPIKE unique id assigned: {}", DescribeUniqueId(a_list));
		}

		void SpikeAdvance(RE::PlayerCharacter* a_player, RE::TESObjectWEAP* a_staff)
		{
			if (const auto* caster = RightHandCaster(a_player); caster && caster->state.underlying() != 0) {
				SpikeNotify("wait until the current cast ends, then press F9 again");
				return;
			}
			if (!pool::IsReady()) {
				SpikeNotify("refused: the plugin is not ready (its frame watch would not run)");
				return;
			}
			EndCastWatch();  // the caster is idle: a cast the frame watch has not closed yet belongs to the old step
			auto* list = WornRightList(a_player, a_staff);
			if (!list || !a_staff->formEnchanting || a_staff->formEnchanting == pool::SharedEnchantment() ||
				(g_spike.staff && (g_spike.staff != a_staff || !IsSpikeItem(list))) ||  // by unique ID: lists can be rebuilt
				(g_spike.step < 0 && list->HasType(RE::ExtraDataType::kEnchantment))) {
				SpikeNotify(std::format("refused for {:08X}: needs an ordinary enchanted staff in the right hand", a_staff->GetFormID()));
				return;
			}
			auto&      slot = a_player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand];
			const auto next = g_spike.step + 1;
			if (next >= static_cast<int>(kSpikeSteps.size())) {
				if (list->HasType(RE::ExtraDataType::kEnchantment)) {
					list->RemoveByType(RE::ExtraDataType::kEnchantment);
				}
				RebindStaff(a_player, a_staff, g_spike.ownSlot, false, "finish");
				// After: RebindStaff writes the record from the slot, which may not be the record's own.
				if (g_spike.ownRecord && a_staff->formEnchanting != g_spike.ownRecord) {
					logger::info("SPIKE record [finish]: {:08X} -> {:08X}", a_staff->formEnchanting ? a_staff->formEnchanting->GetFormID() : 0u,
						g_spike.ownRecord->GetFormID());
					a_staff->formEnchanting = g_spike.ownRecord;
				}
				RestoreSpikeName(list);
				g_magickaEnchantment = nullptr;
				g_stepEnchantment = nullptr;
				g_recordEnchantment = nullptr;
				g_spike = {};
				g_artWatch = {};
				AbortChecks("finished");
				FlushLookups(false);
				SpikeNotify("finished, staff restored");
				return;
			}
			const auto& step = kSpikeSteps[static_cast<std::size_t>(next)];
			auto*                data = RE::TESDataHandler::GetSingleton();
			RE::MagicItem*       spell = nullptr;  // what the art and name come from
			RE::EnchantmentItem* enchantment = nullptr;
			if (step.source == StepSource::kSpellCopy) {
				if (auto* source = data ? data->LookupForm<RE::SpellItem>(step.form, "Skyrim.esm") : nullptr) {
					enchantment = CopySpellToEnchantment(source);
					spell = source;
				}
			} else if (auto* game = data ? data->LookupForm<RE::EnchantmentItem>(step.form, "Skyrim.esm") : nullptr) {
				enchantment = step.source == StepSource::kEnchantmentClone ? CloneEnchantment(game) : game;
				spell = enchantment;
			}
			if (!enchantment) {
				SpikeNotify(std::format("step {} failed: {:06X} or the enchantment is missing", next + 1, step.form));
				return;
			}
			// This step's art must come off and be attached anew (sheathe and draw) before casts count: an art kept from the
			// previous step (same model) would otherwise pass without a new attach.
			g_spike.sawDetach = false;
			g_spike.freshAttach = false;
			if (g_spike.step < 0) {
				g_spike.staff = a_staff;
				g_spike.list = list;
				g_spike.ownSlot = slot;
				g_spike.ownRecord = a_staff->formEnchanting;
				g_spike.hadNameData = list->HasType(RE::ExtraDataType::kTextDisplayData);
				const auto* currentName = list->GetDisplayName(a_staff);
				g_spike.originalName = currentName ? currentName : "";
				EnsureUniqueId(a_player, a_staff, list);
				const auto* id = list->GetByType<RE::ExtraUniqueID>();
				if (!id || id->baseID == 0) {
					// Without its unique ID the item cannot be found again, so nothing is changed on it.
					g_spike = {};
					SpikeNotify(std::format("refused for {:08X}: the item got no unique ID", a_staff->GetFormID()));
					return;
				}
				g_spike.uidBase = id->baseID;
				g_spike.uid = id->uniqueID;
			}
			SetSpikeName(list, a_staff, spell);
			if (list->HasType(RE::ExtraDataType::kEnchantment)) {
				list->RemoveByType(RE::ExtraDataType::kEnchantment);  // nothing goes on the item in the standalone design
			}
			// Before the select, in case it looks up the cost resource.
			g_magickaEnchantment = step.payMagicka ? enchantment : nullptr;
			g_stepEnchantment = enchantment;
			g_recordEnchantment = g_spike.ownRecord;
			// The wanted art is read from the copy, the same source the frame watch judges against.
			if (TipArtOf(enchantment) != TipArtOf(spell)) {
				logger::warn("SPIKE art: copy's main effect art {:08X} differs from the spell's {:08X}", TipArtOf(enchantment) ? TipArtOf(enchantment)->GetFormID() : 0u,
					TipArtOf(spell) ? TipArtOf(spell)->GetFormID() : 0u);
			}
			auto* const oldSlot = slot;
			RebindStaff(a_player, a_staff, enchantment, step.matchRecord, "step");
			ArmSwapCheck(next, oldSlot, enchantment);
			FlushLookups(false);
			g_spike.step = next;
			g_spike.lastSpell = nullptr;
			g_spike.lastState = 0;
			logger::info("SPIKE step {} ench={:08X} dynamic={} cost(player)={:.2f} castType={} chargeTime={:.2f} effects={} record={:08X} payMagicka={} matchRecord={} costHook={} charge={:.2f} magicka={:.2f}",
				next + 1, enchantment->GetFormID(), enchantment->IsDynamicForm(), enchantment->CalculateMagickaCost(a_player), static_cast<int>(enchantment->GetCastingType()),
				enchantment->data.chargeTime, enchantment->effects.size(), a_staff->formEnchanting->GetFormID(), step.payMagicka, step.matchRecord, g_originalResource != nullptr,
				Value(a_player, RE::ActorValue::kRightItemCharge), Value(a_player, RE::ActorValue::kMagicka));
			SpikeNotify(std::format("{}/{}: {}", next + 1, kSpikeSteps.size(), step.label));
		}

		void SpikeFrame(float a_delta)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* caster = player ? player->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand) : nullptr;
			if (!caster) {
				return;
			}
			if (g_spike.step < 0) {
				// Before F9: a cast with the staff's own enchantment is the control (T-ART-CONTROL): it checks the detector.
				const auto* weapon = player->GetEquippedObject(false);
				const auto* staff = weapon ? weapon->As<RE::TESObjectWEAP>() : nullptr;
				if (Enabled() && staff && staff->IsStaff()) {
					WatchArt(static_cast<RE::ActorMagicCaster*>(caster), a_delta);
				} else if (g_artWatch.casting) {
					g_artWatch.valid = false;  // the staff left the hand mid-cast: close the sample as SKIP, start clean
					EndCastWatch();
					g_artWatch = {};
					AbortChecks("the staff left the hand");
				}
				return;
			}
			if (!HoldsSpikeItem(player)) {
				// Unequipped, dropped, sold, or another staff (even of the same kind) taken up: the record must not stay
				// pointed at the copy for every staff of the kind.
				RestoreStaffRecord("staff left the right hand");
				auto* list = FindSpikeList(player);  // found again by unique ID; nullptr when the item is gone
				if (list) {
					RestoreSpikeName(list);
				}
				logger::info("SPIKE stopped: the tested staff left the right hand, name restored={}", list != nullptr);
				g_magickaEnchantment = nullptr;
				g_stepEnchantment = nullptr;
				g_recordEnchantment = nullptr;
				g_spike = {};
				if (g_artWatch.casting) {
					g_artWatch.valid = false;  // closed as SKIP with a line, not dropped
					EndCastWatch();
				}
				g_artWatch = {};
				AbortChecks("the staff left the right hand");
				SpikeNotify("stopped: the staff left the right hand (record restored)");
				return;
			}
			WatchArt(static_cast<RE::ActorMagicCaster*>(caster), a_delta);
			const auto* spell = caster->currentSpell;
			const auto  state = caster->state.underlying();
			if (spell == g_spike.lastSpell && state == g_spike.lastState) {
				return;
			}
			if (state == 0 && g_spike.lastState != 0) {
				// A projectile can land after the caster is idle; its redirect then counts towards the next cast.
				const auto total = g_redirects.load();
				const auto delta = total - g_redirectsReported;
				g_redirectsReported = total;
				const auto misses = g_redirectMisses.load();
				const auto missDelta = misses - g_redirectMissesReported;
				g_redirectMissesReported = misses;
				const auto dynamicEffects = CountDynamicEffects(player);
				const auto detail = std::format("step={} redirects={} misses={} playerEffectsFromCopies={}", g_spike.step + 1, delta, missDelta, dynamicEffects);
				if (delta == 0 && missDelta == 0) {
					selftest::Skip("T-REDIRECT", detail + " (no effect landed in this cast)");
				} else {
					selftest::Report("T-REDIRECT", missDelta == 0 && dynamicEffects == 0, detail);
				}
			}
			g_spike.lastSpell = spell;
			g_spike.lastState = state;
			const auto* slot = player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand];
			const auto* art = static_cast<RE::ActorMagicCaster*>(caster)->castingArt;
			logger::info("SPIKE caster state={} spell={:08X} costNow={:.2f} slot={:08X} charge={:.2f} magicka={:.2f} godMode={} castArt={:08X}", state, spell ? spell->GetFormID() : 0u,
				caster->currentSpellCost, slot ? slot->GetFormID() : 0u, Value(player, RE::ActorValue::kRightItemCharge), Value(player, RE::ActorValue::kMagicka),
				RE::PlayerCharacter::IsGodMode(), art ? art->GetFormID() : 0u);
			if (state == 0) {
				FlushLookups(true);
			}
		}

		void LogSpikeAttackPress()
		{
			if (g_spike.step >= 0) {
				logger::info("SPIKE attack button pressed at step {}", g_spike.step + 1);
				FlushLookups(true, true);  // a refused attempt never returns to state 0, so report it here instead of merging it into the next shot
			}
		}

		void OnBindKey()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* weapon = player ? player->GetEquippedObject(false) : nullptr;
			auto* staff = weapon ? weapon->As<RE::TESObjectWEAP>() : nullptr;
			if (staff && staff->IsStaff() && !keywords::IsConverted(staff->GetFormID())) {
				SpikeAdvance(player, staff);
			} else {
				BindChargeSpell(false);
			}
		}

		class KeySink : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!a_events || !Enabled()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				for (auto* event = *a_events; event; event = event->next) {
					auto* button = event->AsButtonEvent();
					const auto* userEvents = RE::UserEvents::GetSingleton();
					if (button && button->IsDown() && userEvents && button->QUserEvent() == userEvents->rightAttack) {
						if (auto* tasks = SKSE::GetTaskInterface()) {
							tasks->AddTask([] { LogSpikeAttackPress(); });
						}
					}
					if (!button || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard || !button->IsDown()) {
						continue;
					}
					const auto key = button->GetIDCode();
					// Input sinks may run off the main thread: hand the work to the game thread.
					if (auto* tasks = SKSE::GetTaskInterface()) {
						if (key == kKeyBind) {
							if (!pool::IsReady()) {  // OnFrame does not run then, so queued work would wait forever
								tasks->AddTask([] { SpikeNotify("refused: the plugin is not ready (its frame watch would not run)"); });
							} else {
								PostMain([generation = g_resetGeneration.load()] {  // the slot and preload change runs on the main thread
									if (generation != g_resetGeneration.load()) {
										logger::info("SPIKE F9 pressed before a load: dropped");
										return;
									}
									OnBindKey();
								});
							}
						} else if (key == kKeyAttackRight) {
							tasks->AddTask([] { StartAttack(false); });
						} else if (key == kKeyAttackLeft) {
							tasks->AddTask([] { StartAttack(true); });
						}
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		KeySink g_sink;

		class IdentitySink :
			public RE::BSTEventSink<RE::TESEquipEvent>,
			public RE::BSTEventSink<RE::TESUniqueIDChangeEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				if (!a_event || !a_event->equipped || !a_event->actor || !a_event->actor->IsPlayerRef()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// The worn flag is set after this event, so the inventory is read one task later.
				const auto baseObject = a_event->baseObject;
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([baseObject] {
						auto* player = RE::PlayerCharacter::GetSingleton();
						auto* staff = RE::TESForm::LookupByID<RE::TESObjectWEAP>(baseObject);
						if (player && staff && staff->IsStaff()) {
							LogStaffIdentity(player, staff, "equip");
						}
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESUniqueIDChangeEvent* a_event, RE::BSTEventSource<RE::TESUniqueIDChangeEvent>*) override
			{
				if (a_event) {
					logger::info("SPIKE unique id event: object={:08X} ({:08X},{}) -> ({:08X},{})", a_event->objectID, a_event->oldBaseID, a_event->oldUniqueID,
						a_event->newBaseID, a_event->newUniqueID);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		IdentitySink g_identitySink;
	}

	namespace
	{
		// The hand slot after a restart may point at a form that no longer exists (T-SAVE H2), so it is looked up in
		// the form table before it is read. A full scan of the table: only in test builds, at saves and loads.
		std::string DescribeSlot(const RE::MagicItem* a_slot)
		{
			if (!a_slot) {
				return "none";
			}
			bool       registered = false;
			RE::FormID id = 0;
			{
				auto [forms, lock] = RE::TESForm::GetAllForms();
				const RE::BSReadLockGuard guard{ lock.get() };
				if (forms) {
					for (const auto& [formID, form] : *forms) {
						if (form == a_slot) {
							registered = true;
							id = formID;
							break;
						}
					}
				}
			}
			if (!registered) {
				return std::format("{} NOT IN FORM TABLE (not read)", static_cast<const void*>(a_slot));
			}
			return std::format("{:08X} type={} effects={} dynamic={} copy={} name='{}'", id, static_cast<int>(a_slot->GetFormType()), a_slot->effects.size(),
				a_slot->IsDynamicForm(), OriginalOf(a_slot) != nullptr, a_slot->GetName());
		}

		struct TrackedEffect
		{
			RE::FormID    spell{ 0 };
			std::uint32_t index{ 0 };
			float         elapsed{ 0.0f };
			float         duration{ 0.0f };
			float         magnitude{ 0.0f };
			std::uint32_t fromStaff{ 0 };  // 1 = re-pointed by the hook or still on a copy; 0 = e.g. cast by hand
		};

		// Version 2 records every effect of the spike spells with its source, so a lost staff effect cannot be
		// covered by a surviving hand-cast one: all must come back.
		constexpr std::uint32_t kColdRecordVersion = 2;
		constexpr std::uint32_t kMaxTracked = 64;
		constexpr float         kElapsedTolerance = 1.0f;  // the logger and the engine's own write are a few frames apart
		constexpr float         kValueTolerance = 0.01f;

		const std::uint64_t                       g_sessionNonce = std::random_device{}() * 0x100000000ull + std::random_device{}();
		std::optional<std::uint64_t>              g_savedNonce;
		std::vector<TrackedEffect>                g_expected;

		bool IsSpikeSpell(RE::FormID a_id)
		{
			return std::ranges::any_of(kSpikeSteps, [a_id](const SpikeStep& a_step) { return a_step.source == StepSource::kSpellCopy && a_step.form == a_id; });
		}

		// Effects on the player whose source is one of the spike's spells (Skyrim.esm, so the IDs need no load-order
		// fix-up): what the fix should keep across a restart.
		// fromStaff is only known in the game process that cast the effect; after a load it is always 0.
		std::vector<TrackedEffect> CollectTracked(RE::PlayerCharacter* a_player)
		{
			std::vector<TrackedEffect> result;
			auto*                      target = a_player ? a_player->AsMagicTarget() : nullptr;
			auto*                      effects = target ? target->GetActiveEffectList() : nullptr;
			if (!effects) {
				return result;
			}
			for (const auto* effect : *effects) {
				if (!effect || !effect->spell) {
					continue;
				}
				// An effect still on a copy is recorded under its original spell (same index), so losing it after a
				// restart counts as a miss instead of dropping out of the expected list.
				const auto*          copySource = OriginalOf(effect->spell);
				const RE::MagicItem* source = copySource ? copySource : effect->spell;
				if (source->IsDynamicForm() || !IsSpikeSpell(source->GetFormID())) {
					continue;
				}
				bool fromStaff = copySource != nullptr;
				if (!fromStaff) {
					const std::scoped_lock lock{ g_copyLock };
					fromStaff = g_redirectedEffects.contains(effect);
				}
				const auto&   list = effect->spell->effects;
				std::uint32_t index = 0;
				while (index < list.size() && list[index] != effect->effect) {
					++index;
				}
				if (index == list.size()) {
					logger::warn("SPIKE effect of {:08X} is not in its spell's list (half-redirected?); recorded with index {}", source->GetFormID(), index);
				}
				result.push_back({ source->GetFormID(), index, effect->elapsedSeconds, effect->duration, effect->magnitude, fromStaff ? 1u : 0u });
			}
			return result;
		}
	}

	void PrepareSave()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!Enabled() || !player) {
			return;
		}
		if (g_artWatch.casting) {
			g_artWatch.valid = false;  // the save changes the record and slot under a running cast: that sample is dropped
			logger::info("SPIKE save during a cast: art sample dropped");
		}
		AbortChecks("a save changed the hand slot");
		// The record goes back first, whatever the hand slot holds: a save must never see the swapped record's state.
		// What was installed is remembered so the task below can put it back (any step, game enchantment or copy).
		auto* const installed = g_spike.installedRecord;
		const auto  step = g_spike.step;
		RestoreStaffRecord("save");
		auto&       slot = player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand];
		auto* const copy = slot;
		// Any step's enchantment (a copy, a clone or a game enchantment) is taken out of the hand slot while the save is
		// written, so the save never holds a fire staff with another staff's spell in its slot.
		if (!IsSpikeCopy(copy) && !(step >= 0 && copy == g_stepEnchantment.load())) {
			// The staff's own enchantment in the slot is saved as it is; only a matched record goes back after.
			if (installed && step >= 0) {
				if (auto* tasks = SKSE::GetTaskInterface()) {
					PostMain([installed, step] {
						auto* player = RE::PlayerCharacter::GetSingleton();
						const bool same = player && g_spike.step == step && HoldsSpikeItem(player) && g_spike.installedRecord == installed &&
						                  g_spike.staff && g_spike.staff->formEnchanting == g_spike.ownRecord;
						if (same) {
							g_spike.staff->formEnchanting = installed;
							g_spike.afterSave = true;
						}
						logger::info("SPIKE save: staff record {} ({:08X})", same ? "put back to the step's enchantment" : "left as the staff's own",
							installed->GetFormID());
					});
				}
			}
			return;
		}
		// kSaveGame is sent before the engine writes the save (skse64 Hooks_SaveLoad.cpp:25-26), and tasks run after
		// that call returns, so the save holds the staff's own enchantment instead of the copy.
		// The staff record was pointed at the copy (RebindStaff); its own enchantment is the one remembered at the first step.
		auto*                weapon = player->GetEquippedObject(false);
		const auto*          staff = weapon ? weapon->As<RE::TESObjectWEAP>() : nullptr;
		RE::MagicItem* const own = g_spike.ownRecord ? g_spike.ownRecord : (staff ? staff->formEnchanting : nullptr);
		// A raw write: the save handler's thread is not proven to be the main thread, so the engine's preload calls are
		// made only in the task below (main thread). Until then the copy keeps its preload reference and own has none.
		slot = own;
		logger::info("SPIKE save: hand slot {:08X} -> {:08X} while the save is written (thread {})", copy->GetFormID(), own ? own->GetFormID() : 0u,
			REX::W32::GetCurrentThreadId());
		// Only the hand slot is covered. No current step puts the copy on the item; the check stays for a step that would.
		const auto* item = FindSpikeList(player);  // found again by unique ID: the remembered list may be stale
		if (const auto* extra = item ? item->GetByType<RE::ExtraEnchantment>() : nullptr; extra && OriginalOf(extra->enchantment)) {
			logger::warn("SPIKE save: the worn staff still carries copy {:08X}; this save is not a valid T-SAVE sample", extra->enchantment->GetFormID());
		}
		if (auto* tasks = SKSE::GetTaskInterface()) {
			PostMain([copy, own] {
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!player) {
					return;
				}
				auto& slot = player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand];
				// When the copy does not go back, its preload reference moves to whatever stays: own, if our raw write is
				// still there; otherwise the engine (or a load) already set the slot and preloaded its item.
				const auto settle = [&](const char* a_why) {
					const bool ownStays = slot == own;
					if (PreloadInstalled()) {
						if (ownStays && own) {
							g_preload(own);
						}
						g_releasePreload(copy, true);
					}
					logger::warn("SPIKE save: copy {:08X} not put back ({}); preload moved to {} (thread {})", copy->GetFormID(), a_why,
						ownStays ? "the staff's own enchantment" : "the engine's slot item", REX::W32::GetCurrentThreadId());
				};
				if (g_stepEnchantment.load() != copy) {
					settle("spike reset or moved on before the put-back");
					return;
				}
				auto* enchantment = copy->As<RE::EnchantmentItem>();
				// Both or neither: the record and the slot must match (the T-ART hypothesis), on the same held item.
				const bool sameItem = HoldsSpikeItem(player);
				const bool recordFree = g_spike.staff && g_spike.staff->formEnchanting == g_spike.ownRecord;
				if (slot == own && sameItem && recordFree && enchantment) {
					const bool recordBack = g_spike.installedRecord == enchantment;
					if (recordBack) {
						g_spike.staff->formEnchanting = enchantment;  // only when this step matched the record; record before slot
					}
					slot = copy;  // raw: the copy kept its preload reference through the save
					g_spike.afterSave = true;
					logger::info("SPIKE save: hand slot back to {:08X}, staff record {:08X} ({})", copy->GetFormID(),
						g_spike.staff->formEnchanting ? g_spike.staff->formEnchanting->GetFormID() : 0u, recordBack ? "matched again" : "its own");
				} else if (slot == own) {
					logger::warn("SPIKE save: sameItem={} recordFree={} enchantment={}; slot and record stay the staff's own", sameItem, recordFree,
						enchantment != nullptr);
					settle("the held item or record changed");
				} else {
					logger::warn("SPIKE save: hand slot changed during the save ({:08X})", slot ? slot->GetFormID() : 0u);
					settle("the slot changed during the save");
				}
			});
		}
	}

	void WriteSaveRecord(SKSE::SerializationInterface* a_intfc)
	{
		if (!Enabled()) {
			return;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		// Written from inside the save itself: shows what the save really holds in the hand slot (PrepareSave check).
		logger::info("SELFTEST T-SAVE-COLD hand slot as saved: {}",
			DescribeSlot(player ? player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand] : nullptr));
		auto tracked = CollectTracked(player);
		if (tracked.size() > kMaxTracked) {
			// A cut list could let surviving hand-cast effects stand in for a lost staff one, so the sample is dropped.
			logger::warn("SELFTEST T-SAVE-COLD not armed: {} effects of the spike spells, more than {}", tracked.size(), kMaxTracked);
			tracked.clear();
		}
		const auto count = static_cast<std::uint32_t>(tracked.size());  // 0 is written too, so the load reports SKIP
		if (!a_intfc->OpenRecord(kColdSaveRecord, kColdRecordVersion) || !a_intfc->WriteRecordData(g_sessionNonce) || !a_intfc->WriteRecordData(count) ||
			!a_intfc->WriteRecordData(tracked.data(), static_cast<std::uint32_t>(sizeof(TrackedEffect) * tracked.size()))) {
			logger::error("SELFTEST T-SAVE-COLD: writing the co-save record failed");
			return;
		}
		logger::info("SELFTEST T-SAVE-COLD armed: session={:016X} effects={}", g_sessionNonce, count);
	}

	void ReadSaveRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length)
	{
		g_savedNonce.reset();
		g_expected.clear();
		std::uint64_t nonce = 0;
		std::uint32_t count = 0;
		constexpr auto header = static_cast<std::uint32_t>(sizeof(nonce) + sizeof(count));
		if (a_version != kColdRecordVersion || a_length < header || a_intfc->ReadRecordData(nonce) != sizeof(nonce) ||
			a_intfc->ReadRecordData(count) != sizeof(count) || count > kMaxTracked || a_length != header + count * sizeof(TrackedEffect)) {
			logger::error("SELFTEST T-SAVE-COLD: co-save record unreadable (version {}, length {}, count {})", a_version, a_length, count);
			return;
		}
		g_expected.resize(count);
		const auto bytes = static_cast<std::uint32_t>(count * sizeof(TrackedEffect));
		if (a_intfc->ReadRecordData(g_expected.data(), bytes) != bytes) {
			logger::error("SELFTEST T-SAVE-COLD: co-save record truncated");
			g_expected.clear();
			return;
		}
		for (auto& effect : g_expected) {
			RE::FormID current = 0;
			effect.spell = a_intfc->ResolveFormID(effect.spell, current) ? current : 0;
		}
		g_savedNonce = nonce;
	}

	void RevertSaveRecord()
	{
		RestoreStaffRecord("revert");
		g_savedNonce.reset();
		g_expected.clear();
	}

	void EvaluateColdLoad(bool a_loadSucceeded)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!Enabled() || !player) {
			return;
		}
		if (!a_loadSucceeded) {
			selftest::Skip("T-SAVE-COLD", "the load failed");
			return;
		}
		if (!g_savedNonce) {
			return;  // the save has no record: made without this test build
		}
		const auto staffEffects = std::ranges::count_if(g_expected, [](const TrackedEffect& a_effect) { return a_effect.fromStaff != 0; });
		if (staffEffects == 0) {
			selftest::Skip("T-SAVE-COLD", std::format("the save held no effect the staff produced ({} others)", g_expected.size()));
			return;
		}
		auto        current = CollectTracked(player);
		std::size_t matched = 0;
		for (const auto& want : g_expected) {
			const auto hit = std::ranges::find_if(current, [&want](const TrackedEffect& a_have) {
				return a_have.spell != 0 && a_have.spell == want.spell && a_have.index == want.index && std::fabs(a_have.duration - want.duration) < kValueTolerance &&
				       std::fabs(a_have.magnitude - want.magnitude) < kValueTolerance && std::fabs(a_have.elapsed - want.elapsed) < kElapsedTolerance;
			});
			if (hit == current.end()) {
				logger::info("SELFTEST T-SAVE-COLD missing: spell={:08X}[{}] fromStaff={} elapsed={:.1f} duration={:.1f} magnitude={:.1f}", want.spell, want.index,
					want.fromStaff, want.elapsed, want.duration, want.magnitude);
				continue;
			}
			hit->spell = 0;  // used
			++matched;
		}
		const bool restarted = *g_savedNonce != g_sessionNonce;
		const auto dynamicEffects = CountDynamicEffects(player);
		const auto detail = std::format("restarted={} expected={} (staff {}) matched={} playerEffectsFromCopies={}", restarted, g_expected.size(), staffEffects, matched,
			dynamicEffects);
		if (!restarted) {
			selftest::Skip("T-SAVE-COLD", detail + " (same game process: proves nothing about a restart)");
			return;
		}
		selftest::Report("T-SAVE-COLD", matched == g_expected.size(), detail);
	}

	void LogSaveState(const char* a_when)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!Enabled() || !player) {
			return;
		}
		const auto* slot = player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand];
		const auto* caster = RightHandCaster(player);
		const auto* cast = caster ? caster->currentSpell : nullptr;
		logger::info("SPIKE state [{}] slot={} casterSpell={:08X}", a_when, DescribeSlot(slot), cast ? cast->GetFormID() : 0u);
		for (const auto& effect : CollectTracked(player)) {
			logger::info("SPIKE state [{}] spike-spell effect: spell={:08X}[{}] fromStaff={} elapsed={:.1f} duration={:.1f} magnitude={:.1f}", a_when, effect.spell,
				effect.index, effect.fromStaff, effect.elapsed, effect.duration, effect.magnitude);
		}
		std::size_t total = 0;
		std::size_t dynamicSpells = 0;
		if (auto* target = player->AsMagicTarget(); auto* effects = target ? target->GetActiveEffectList() : nullptr) {
			for (const auto* effect : *effects) {
				if (!effect) {
					continue;
				}
				++total;
				if (effect->spell && effect->spell->IsDynamicForm()) {
					++dynamicSpells;
					logger::info("SPIKE state [{}] active effect: spell={:08X} elapsed={:.1f} duration={:.1f} magnitude={:.1f}", a_when, effect->spell->GetFormID(),
						effect->elapsedSeconds, effect->duration, effect->magnitude);
				}
			}
		}
		logger::info("SPIKE state [{}] active effects total={} from runtime spells={}", a_when, total, dynamicSpells);
		auto* weapon = player->GetEquippedObject(false);
		if (auto* staff = weapon ? weapon->As<RE::TESObjectWEAP>() : nullptr; staff && staff->IsStaff()) {
			LogStaffIdentity(player, staff, a_when);
		}
	}

	void Install()
	{
		if (!Enabled()) {
			return;
		}
		if (auto* devices = RE::BSInputDeviceManager::GetSingleton()) {
			devices->AddEventSink(&g_sink);
			logger::warn("TEST CONTROLS ON (F9 bind, F10 right attack, F11 left attack); development builds only");
		}
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESEquipEvent>(&g_identitySink);
			holder->AddEventSink<RE::TESUniqueIDChangeEvent>(&g_identitySink);
		}
		InstallResourceHook();
		InstallAddTargetHooks();
		InstallPreload();
	}

	void ResetSpike()
	{
		g_resetGeneration.fetch_add(1);
		{
			const std::scoped_lock lock{ g_copyLock };
			g_redirectedEffects.clear();  // the effects they named belong to the game being left
		}
		RestoreStaffRecord("reset");  // records are not reloaded with a save: the swapped enchantment would stay
		g_magickaEnchantment = nullptr;
		g_stepEnchantment = nullptr;
		g_recordEnchantment = nullptr;
		g_spike = {};
		if (g_artWatch.casting) {
			g_artWatch.valid = false;
			EndCastWatch();
		}
		g_artWatch = {};
		AbortChecks("reset (load or new game)");
		FlushLookups(false);
	}

	void OnFrame(float a_delta)
	{
		if (std::isfinite(a_delta) && a_delta > 0.0f) {
			g_artWatch.clock += a_delta;
		}
		DrainMain();
		SpikeFrame(a_delta);
		if (!g_hold.active || !std::isfinite(a_delta)) {
			return;
		}
		g_hold.elapsed += a_delta;
		if (g_hold.elapsed < g_hold.duration) {
			Push(g_hold.left, 1.0f, g_hold.elapsed);  // held events every frame, like a real button
			return;
		}
		Push(g_hold.left, 0.0f, g_hold.elapsed);
		logger::info("TEST attack {} released after {:.2f}s", g_hold.left ? "L" : "R", g_hold.elapsed);
		g_hold = {};
	}
}
