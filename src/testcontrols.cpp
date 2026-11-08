#include "PCH.h"

#include "testcontrols.h"

#include "conversion.h"
#include "detour_helper.h"
#include "keywords.h"
#include "pool.h"
#include "settings.h"

#include <atomic>
#include <intrin.h>
#include <mutex>

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
		constexpr RE::FormID kSpikeIceSpike = 0x2B96C;  // fire-and-forget
		constexpr RE::FormID kSpikeFlames = 0x7E881;    // crAtronachFlameFlames: concentration. In this load order NoviceBoltSpell-Mysticism.esp turns Sparks, Flames and Frostbite into fire-and-forget

		struct SpikeStep
		{
			RE::FormID  spell;
			bool        payMagicka;
			bool        attachToItem;
			bool        emptyCharge;
			const char* label;
		};
		constexpr RE::FormID kSpikeOakflesh = 0x5AD5C;  // self buff with a duration: leaves an ActiveEffect that references the copy

		constexpr std::array kSpikeSteps{
			SpikeStep{ kSpikeIceSpike, false, true, false, "Ice Spike, paid from staff charge" },
			SpikeStep{ kSpikeFlames, false, true, false, "Flames (concentration), paid from staff charge" },
			SpikeStep{ kSpikeIceSpike, true, true, false, "Ice Spike, paid from magicka" },
			SpikeStep{ kSpikeIceSpike, true, false, false, "Ice Spike, magicka, nothing on the item" },
			SpikeStep{ kSpikeFlames, true, false, false, "Flames (concentration), magicka, nothing on the item" },
			SpikeStep{ kSpikeIceSpike, true, false, true, "Ice Spike, magicka, staff charge empty" },
			SpikeStep{ kSpikeOakflesh, true, false, false, "Oakflesh (self buff), magicka, nothing on the item" },
		};

		struct SpikeState
		{
			int                  step{ -1 };
			RE::TESObjectWEAP*   staff{ nullptr };
			RE::ExtraDataList*   list{ nullptr };  // the exact item under test
			RE::MagicItem*       ownSlot{ nullptr };
			RE::BGSArtObject*    ownCastingArt{ nullptr };
			RE::EffectSetting*   recordEffect{ nullptr };  // the staff record's main effect while its art is borrowed
			RE::BGSArtObject*    recordCastingArt{ nullptr };
			RE::TESObjectLIGH*   recordLight{ nullptr };
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

		RE::ActorValue ResourceThunk(RE::MagicItem* a_item, RE::MagicSystem::CastingSource a_source)
		{
			const auto engineValue = g_originalResource(a_item, a_source);
			const auto returnedValue = a_item && a_item == g_magickaEnchantment.load() ? RE::ActorValue::kMagicka : engineValue;
			if (a_item && (a_item == g_stepEnchantment.load() || a_item == g_recordEnchantment.load())) {
				RecordLookup({ reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - REL::Module::get().base(), a_item->GetFormID(),
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

		RE::EnchantmentItem* CopySpellToEnchantment(const RE::SpellItem* a_spell)
		{
			auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::EnchantmentItem>();
			auto* enchantment = factory ? factory->Create() : nullptr;
			if (!enchantment) {
				return nullptr;
			}
			enchantment->AddChange(1);
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
			return enchantment;
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

		// The hand caster keeps the charging art it picked when the staff was equipped, so the tip needs the new one
		// here. Same write as staff_art::RefreshCastingArt, confirmed in game on 2026-11-07. SpikeAdvance only runs while idle.
		void SetCastingArt(RE::PlayerCharacter* a_player, RE::BGSArtObject* a_art)
		{
			auto* caster = RightHandCaster(a_player);
			if (caster) {
				caster->castingArt = a_art;
			}
			logger::info("SPIKE casting art -> {:08X} applied={}", a_art ? a_art->GetFormID() : 0u, caster != nullptr);
		}

		void RestoreRecordArt()
		{
			if (g_spike.recordEffect) {
				g_spike.recordEffect->data.castingArt = g_spike.recordCastingArt;
				g_spike.recordEffect->data.light = g_spike.recordLight;
				g_spike.recordEffect = nullptr;
			}
		}

		// Hypothesis under test: the tip shows the art of the staff record's enchantment, not of the spell in the hand.
		// The SCS path changed the tip the same way (staff_art::Write on the enchantment the record pointed at). While
		// this runs, anything using this effect shows the borrowed art; every staff of the kind shares it.
		void SetRecordArt(const RE::TESObjectWEAP* a_staff, const RE::EffectSetting* a_source)
		{
			const auto* main = a_staff->formEnchanting ? a_staff->formEnchanting->GetCostliestEffectItem() : nullptr;
			auto*       effect = main ? main->baseEffect : nullptr;
			if (!effect || !a_source) {
				logger::warn("SPIKE record art skipped: effect={} source={}", effect != nullptr, a_source != nullptr);
				return;
			}
			if (g_spike.recordEffect != effect) {
				RestoreRecordArt();
				g_spike.recordEffect = effect;
				g_spike.recordCastingArt = effect->data.castingArt;
				g_spike.recordLight = effect->data.light;
			}
			effect->data.castingArt = a_source->data.castingArt;
			effect->data.light = a_source->data.light;
			logger::info("SPIKE record effect {:08X} art -> {:08X} (its own {:08X})", effect->GetFormID(),
				a_source->data.castingArt ? a_source->data.castingArt->GetFormID() : 0u, g_spike.recordCastingArt ? g_spike.recordCastingArt->GetFormID() : 0u);
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

		void SetSpikeName(RE::ExtraDataList* a_list, const RE::TESObjectWEAP* a_staff, const RE::SpellItem* a_spell)
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
			auto* list = WornRightList(a_player, a_staff);
			if (!list || !a_staff->formEnchanting || a_staff->formEnchanting == pool::SharedEnchantment() ||
				(g_spike.staff && (g_spike.staff != a_staff || g_spike.list != list)) ||
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
				slot = g_spike.ownSlot;
				SetCastingArt(a_player, g_spike.ownCastingArt);
				RestoreRecordArt();
				RestoreSpikeName(list);
				g_magickaEnchantment = nullptr;
				g_stepEnchantment = nullptr;
				g_recordEnchantment = nullptr;
				g_spike = {};
				FlushLookups(false);
				SpikeNotify("finished, staff restored (its charge stays empty)");
				return;
			}
			const auto& step = kSpikeSteps[static_cast<std::size_t>(next)];
			auto*       data = RE::TESDataHandler::GetSingleton();
			auto*       spell = data ? data->LookupForm<RE::SpellItem>(step.spell, "Skyrim.esm") : nullptr;
			auto*       enchantment = spell ? CopySpellToEnchantment(spell) : nullptr;
			if (!enchantment) {
				SpikeNotify(std::format("step {} failed: spell {:06X} or the new enchantment is missing", next + 1, step.spell));
				return;
			}
			if (g_spike.step < 0) {
				g_spike.staff = a_staff;
				g_spike.list = list;
				g_spike.ownSlot = slot;
				const auto* caster = RightHandCaster(a_player);
				g_spike.ownCastingArt = caster ? caster->castingArt : nullptr;
				g_spike.hadNameData = list->HasType(RE::ExtraDataType::kTextDisplayData);
				const auto* currentName = list->GetDisplayName(a_staff);
				g_spike.originalName = currentName ? currentName : "";
				EnsureUniqueId(a_player, a_staff, list);
			}
			SetSpikeName(list, a_staff, spell);
			if (!step.attachToItem) {
				if (list->HasType(RE::ExtraDataType::kEnchantment)) {
					list->RemoveByType(RE::ExtraDataType::kEnchantment);
				}
			} else if (auto* extra = list->GetByType<RE::ExtraEnchantment>()) {
				extra->enchantment = enchantment;
			} else {
				list->SetEnchantment(enchantment, static_cast<std::uint16_t>(a_staff->amountofEnchantment), false);
			}
			if (step.emptyCharge) {
				if (auto* values = a_player->AsActorValueOwner()) {
					values->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kRightItemCharge,
						-values->GetActorValue(RE::ActorValue::kRightItemCharge));
				}
			}
			slot = enchantment;
			const auto* mainEffect = spell->GetCostliestEffectItem();
			const auto* artSource = mainEffect ? mainEffect->baseEffect : nullptr;
			SetCastingArt(a_player, artSource ? artSource->data.castingArt : nullptr);
			SetRecordArt(a_staff, artSource);
			g_magickaEnchantment = step.payMagicka ? enchantment : nullptr;
			g_stepEnchantment = enchantment;
			g_recordEnchantment = a_staff->formEnchanting;
			FlushLookups(false);
			g_spike.step = next;
			g_spike.lastSpell = nullptr;
			g_spike.lastState = 0;
			logger::info("SPIKE step {} ench={:08X} cost(player)={:.2f} castType={} chargeTime={:.2f} effects={} record={:08X} payMagicka={} onItem={} costHook={} charge={:.2f} magicka={:.2f}",
				next + 1, enchantment->GetFormID(), enchantment->CalculateMagickaCost(a_player), static_cast<int>(enchantment->GetCastingType()), enchantment->data.chargeTime, enchantment->effects.size(),
				a_staff->formEnchanting->GetFormID(), step.payMagicka, step.attachToItem, g_originalResource != nullptr, Value(a_player, RE::ActorValue::kRightItemCharge),
				Value(a_player, RE::ActorValue::kMagicka));
			SpikeNotify(std::format("{}/{}: {}", next + 1, kSpikeSteps.size(), step.label));
		}

		void SpikeFrame()
		{
			if (g_spike.step < 0) {
				return;
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* caster = player ? player->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand) : nullptr;
			if (!caster) {
				return;
			}
			const auto* spell = caster->currentSpell;
			const auto  state = caster->state.underlying();
			if (spell == g_spike.lastSpell && state == g_spike.lastState) {
				return;
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
							tasks->AddTask([] { OnBindKey(); });
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

	void LogSaveState(const char* a_when)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!Enabled() || !player) {
			return;
		}
		const auto* slot = player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand];
		const auto* caster = RightHandCaster(player);
		const auto* cast = caster ? caster->currentSpell : nullptr;
		logger::info("SPIKE state [{}] slot={:08X} '{}' casterSpell={:08X}", a_when, slot ? slot->GetFormID() : 0u, slot ? slot->GetName() : "", cast ? cast->GetFormID() : 0u);
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
	}

	void ResetSpike()
	{
		RestoreRecordArt();
		g_magickaEnchantment = nullptr;
		g_stepEnchantment = nullptr;
		g_recordEnchantment = nullptr;
		g_spike = {};
		FlushLookups(false);
	}

	void OnFrame(float a_delta)
	{
		SpikeFrame();
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
