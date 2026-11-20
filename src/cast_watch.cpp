#include "cast_watch.h"

#include "api.h"
#include "copy_traits.h"
#include "core/spell_rules.h"
#include "effect_redirect.h"
#include "hand.h"
#include "selftest.h"
#include "settings.h"
#include "spell_copy.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace scssc::cast_watch
{
	namespace
	{
		constexpr auto  kFirstSkillValue = RE::ActorValue::kOneHanded;  // PlayerSkills index 0
		constexpr float kPayShare = 0.5f;                               // the drop must reach half the cost (regen, perks)
		constexpr float kChargeTolerance = 0.01f;
		constexpr float kMinStreamSecs = 0.5f;                          // shorter concentration samples are not judged
		constexpr float kCostTolerance = 0.01f;                         // T-COST: copy and hand cost may differ by 1 %
		constexpr float kDrainTolerance = 0.05f;                        // T-COST: a single shot drains the hand cost within 5 %
		constexpr float kXpCheckSecs = 1.0f;                            // the granted experience is read again after this

		// Player casts of each staff copy, counted by the game's own spell-cast event (it names no hand).
		std::mutex                                 g_castEventLock;
		std::unordered_map<RE::FormID, std::uint32_t> g_copyCastEvents;

		std::uint32_t CastEventsOf(const RE::MagicItem* a_item)
		{
			const std::scoped_lock lock{ g_castEventLock };
			const auto it = a_item ? g_copyCastEvents.find(a_item->GetFormID()) : g_copyCastEvents.end();
			return it == g_copyCastEvents.end() ? 0 : it->second;
		}

		struct XpCheck
		{
			bool           active{ false };
			RE::ActorValue school{ RE::ActorValue::kNone };
			float          before{ 0.0f };
			float          levelBefore{ 0.0f };
			float          use{ 0.0f };
			float          due{ 0.0f };
			RE::FormID     item{ 0 };
			bool           hand{ false };
		};
		XpCheck g_xpCheck;
		float   g_clock = 0.0f;

		struct Sample
		{
			bool                 active{ false };
			bool                 plain{ false };  // a cast of a plain spell from the hand, watched only for the T-XP comparison
			Hand                 side{ Hand::kRight };
			const RE::MagicItem* item{ nullptr };
			RE::SpellItem*       spell{ nullptr };  // the copy's original, or the hand spell
			RE::ActorValue       school{ RE::ActorValue::kNone };
			float                cost{ 0.0f };
			float                startMagicka{ 0.0f };
			float                peakMagicka{ 0.0f };
			float                deepestDrop{ 0.0f };
			float                startCharge{ 0.0f };
			float                largestChargeChange{ 0.0f };
			float                startXp{ 0.0f };
			float                startLevel{ 0.0f };
			float                streamSecs{ 0.0f };
			std::uint32_t        rangeAsksAtStart{ 0 };
			std::uint32_t        castEventsAtStart{ 0 };
			std::uint32_t        effectsAtStart{ 0 };
			std::uint32_t        unhandedEffectsAtStart{ 0 };
			const char*          releasedBy{ "" };  // the first signal that showed the release, for the log
			bool                 announced{ false };  // the SCST message went out for this cast
			bool                 released{ false };
			bool                 concentration{ false };
			bool                 otherHandCast{ false };  // the other hand cast too: the drop is not ours alone
			bool                 sharedCopy{ false };     // the other hand cast the same copy during this sample: its event is not ours
			bool                 slotChanged{ false };
		};
		std::array<Sample, 2> g_samples;  // per hand

		float Value(RE::PlayerCharacter* a_player, RE::ActorValue a_value)
		{
			auto* values = a_player->AsActorValueOwner();
			return values ? values->GetActorValue(a_value) : 0.0f;
		}

		const RE::PlayerCharacter::PlayerSkills::Data::SkillData* Skill(RE::PlayerCharacter* a_player, RE::ActorValue a_school)
		{
			const auto index = static_cast<int>(a_school) - static_cast<int>(kFirstSkillValue);
			auto*      skills = a_player->GetInfoRuntimeData().skills;
			if (!skills || !skills->data || index < 0 || index >= RE::PlayerCharacter::PlayerSkills::Data::Skill::kTotal) {
				return nullptr;
			}
			return &skills->data->skills[index];
		}

		float SchoolXp(RE::PlayerCharacter* a_player, RE::ActorValue a_school)
		{
			const auto* skill = Skill(a_player, a_school);
			return skill ? skill->xp : 0.0f;
		}

		float SchoolLevel(RE::PlayerCharacter* a_player, RE::ActorValue a_school)
		{
			const auto* skill = Skill(a_player, a_school);
			return skill ? skill->level : 0.0f;
		}

		// The grant is judged one second later as well, in case the game applies it after the call returns.
		void JudgeXp(RE::PlayerCharacter* a_player)
		{
			auto& c = g_xpCheck;
			if (!c.active || g_clock < c.due) {
				return;
			}
			c.active = false;
			const float gained = SchoolXp(a_player, c.school) - c.before;
			const bool  leveled = SchoolLevel(a_player, c.school) > c.levelBefore;
			const auto  detail = std::format("{} {:08X} AV {}: xp {:+.3f} from the cast's start to one second after it{}", c.hand ? "hand cast of" : "staff copy", c.item,
				 static_cast<int>(c.school), gained, leveled ? " (level went up)" : "");
			if (c.hand) {
				logger::info("T-XP {}", detail);  // the reference for the staff line
			} else if (gained > 0.0f || leveled) {
				selftest::Report("T-XP", true, detail);
			} else {
				// Destruction and runes train on the hit, not the cast: compare with the hand cast line instead of failing.
				selftest::Skip("T-XP", detail + " (no xp within a second; spells that train on a hit or a trigger show it later)");
			}
		}

		RE::MagicCaster::State StateOf(RE::PlayerCharacter* a_player, RE::MagicSystem::CastingSource a_source)
		{
			const auto* caster = a_player->GetMagicCaster(a_source);
			return caster ? caster->state.get() : RE::MagicCaster::State::kNone;
		}

		void Start(RE::PlayerCharacter* a_player, Hand a_side, RE::MagicItem* a_item, RE::SpellItem* a_spell, bool a_plain)
		{
			auto& s = g_samples[Index(a_side)];
			s = {};
			s.active = true;
			s.plain = a_plain;
			s.side = a_side;
			s.item = a_item;
			s.spell = a_spell;
			s.school = a_spell->GetAssociatedSkill();
			s.cost = a_item->CalculateMagickaCost(a_player);
			s.startMagicka = Value(a_player, RE::ActorValue::kMagicka);
			s.peakMagicka = s.startMagicka;
			s.startCharge = Value(a_player, ChargeValue(a_side));
			s.startXp = SchoolXp(a_player, s.school);
			s.startLevel = SchoolLevel(a_player, s.school);
			s.rangeAsksAtStart = copy_traits::RangeAsks();
			s.castEventsAtStart = CastEventsOf(a_item);
			s.effectsAtStart = effect_redirect::CopyApplications(a_side);
			s.unhandedEffectsAtStart = effect_redirect::CopyApplicationsWithoutHand();
			s.concentration = a_item->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
		}

		const char* SkipReason(const Sample& a_s)
		{
			if (a_s.slotChanged) {
				return "the hand slot changed during the cast";
			}
			if (!a_s.released) {
				return "the cast was not released";
			}
			if (a_s.cost < 1.0f) {
				return "cost below 1";
			}
			if (RE::PlayerCharacter::IsGodMode()) {
				return "god mode (tgm) pays nothing";
			}
			if (a_s.otherHandCast) {
				return "the other hand cast at the same time";
			}
			if (a_s.startMagicka < a_s.cost) {
				return "magicka at the start was below the cost";
			}
			if (a_s.concentration && a_s.streamSecs < kMinStreamSecs) {
				return "concentration stream shorter than 0.5 s";
			}
			return nullptr;
		}

		// The engine trains the school itself now (copy_traits answers GetSkillUsageData with the spell's data). Judged one
		// second after the cast from the xp at its start. A check still running is kept, not replaced (Codex 2026-11-17).
		void ArmXpCheck(RE::PlayerCharacter* a_player, const Sample& a_s)
		{
			if (g_xpCheck.active || !a_s.released || a_s.slotChanged || a_s.otherHandCast || a_s.school == RE::ActorValue::kNone) {
				return;  // another cast's check is running, or the left hand may train the same school: not judged
			}
			g_xpCheck = { true, a_s.school, a_s.startXp, a_s.startLevel, 0.0f, g_clock + kXpCheckSecs, a_s.item->GetFormID(), a_s.plain };
		}

		void FinishCopy(RE::PlayerCharacter* a_player, const Sample& a_s)
		{
			const auto detail = std::format("{} hand copy {:08X} cost {:.2f}{}: deepest magicka drop {:.2f}, staff charge change {:.2f}", Name(a_s.side), a_s.item->GetFormID(), a_s.cost,
				a_s.concentration ? std::format(" per second over {:.2f} s", a_s.streamSecs) : std::string{}, a_s.deepestDrop, a_s.largestChargeChange);
			if (const auto* reason = SkipReason(a_s)) {
				selftest::Skip("T-PAY", std::format("{} ({})", detail, reason));
			} else {
				// Same bar as the test build (a560bf0 testcontrols.cpp:991-1001): half the cost, per second for a stream.
				const float expected = (a_s.concentration ? a_s.cost * std::min(a_s.streamSecs, 1.0f) : a_s.cost) * kPayShare;
				selftest::Report("T-PAY", a_s.deepestDrop >= expected && a_s.largestChargeChange <= kChargeTolerance, detail);
			}
			if (a_s.released && !a_s.slotChanged) {
				const float handCost = a_s.spell->CalculateMagickaCost(a_player);
				// For a single shot the magicka actually drained must match too, not only the reported cost.
				const bool drainChecked = !a_s.concentration && !SkipReason(a_s);
				const bool drainOk = !drainChecked || std::fabs(a_s.deepestDrop - handCost) <= std::max(handCost, 1.0f) * kDrainTolerance;
				selftest::Report("T-COST", std::fabs(a_s.cost - handCost) <= std::max(handCost, 1.0f) * kCostTolerance && drainOk,
					std::format("copy {:08X} cost {:.2f}, drained {:.2f}{}, spell {:08X} by hand {:.2f}", a_s.item->GetFormID(), a_s.cost, a_s.deepestDrop,
						drainChecked ? "" : " (drain not judged)", a_s.spell->GetFormID(), handCost));
				if (a_s.spell->GetRange() != 0.0f) {
					const auto asks = copy_traits::RangeAsks() - a_s.rangeAsksAtStart;
					selftest::Report("T-RANGE", asks > 0,
						std::format("copy {:08X} of ranged spell {:08X} (range {:.1f}): asked {} time(s) during this cast; where it landed is the player's check", a_s.item->GetFormID(),
							a_s.spell->GetFormID(), a_s.spell->GetRange(), asks));
				}
			}
			ArmXpCheck(a_player, a_s);
		}

		// The game sends no TESSpellCastEvent for a staff (enchantment) cast: Heart of Magic's probe saw events for hand
		// casts only (2026-11-18, SpellLearning.log STAFF-PROBE), so plugins that count casts never saw the staff. Each
		// released copy cast is broadcast as an SKSE message instead (api.cpp has the format), once per cast.
		void AnnounceStaffCast(const Sample& a_s)
		{
			auto* messaging = SKSE::GetMessagingInterface();
			if (!messaging) {
				return;
			}
			StaffCastMessage message{ a_s.item->GetFormID(), a_s.spell->GetFormID(), a_s.concentration ? 1u : 0u, static_cast<std::uint32_t>(Index(a_s.side)) };
			const bool sent = messaging->Dispatch(kStaffCastMessageType, &message, sizeof(message), nullptr);
			logger::debug("staff cast message: {} hand copy {:08X} spell {:08X} concentration={} released by {} ({})", Name(a_s.side), message.copy, message.spell,
				message.concentration, a_s.releasedBy, sent ? "sent" : "NOT sent");
		}

		void FinishHand(RE::PlayerCharacter* a_player, const Sample& a_s)
		{
			ArmXpCheck(a_player, a_s);  // logged the same way, to compare with a staff cast of the same spell
		}
	}

	void Reset()
	{
		g_samples = {};
		g_xpCheck = {};
	}

	void InstallEvents()
	{
		struct Sink final : RE::BSTEventSink<RE::TESSpellCastEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event, RE::BSTEventSource<RE::TESSpellCastEvent>*) override
			{
				if (a_event && a_event->object && a_event->object->IsPlayerRef() && spell_copy::IsCopy(RE::TESForm::LookupByID<RE::MagicItem>(a_event->spell))) {
					const std::scoped_lock lock{ g_castEventLock };
					++g_copyCastEvents[a_event->spell];
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		static Sink sink;
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESSpellCastEvent>(&sink);
		}
	}

	namespace
	{
		void WatchHand(RE::PlayerCharacter* a_player, Hand a_side, float a_delta)
		{
			auto* const slot = a_player->GetActorRuntimeData().selectedSpells[SlotType(a_side)];
			const auto  state = StateOf(a_player, Source(a_side));
			auto&       s = g_samples[Index(a_side)];
			if (!s.active) {
				if (state == RE::MagicCaster::State::kNone || !slot) {
					return;
				}
				if (auto* original = spell_copy::OriginalOf(slot)) {
					Start(a_player, a_side, slot, original, false);
				} else if (auto* handSpell = slot->As<RE::SpellItem>(); handSpell && Settings::Get().selfTest) {
					Start(a_player, a_side, slot, handSpell, true);
				}
				if (!s.active) {
					return;
				}
				// No return here: a shot that starts and releases in one frame is still seen as released (Carbon 2026-11-17).
			}
			const float magicka = Value(a_player, RE::ActorValue::kMagicka);
			s.peakMagicka = std::max(s.peakMagicka, magicka);
			s.deepestDrop = std::max(s.deepestDrop, s.peakMagicka - magicka);
			s.largestChargeChange = std::max(s.largestChargeChange, std::fabs(Value(a_player, ChargeValue(a_side)) - s.startCharge));
			s.otherHandCast |= StateOf(a_player, Source(Other(a_side))) != RE::MagicCaster::State::kNone;
			s.slotChanged |= slot != s.item;
			if (state == RE::MagicCaster::State::kCasting && std::isfinite(a_delta) && a_delta > 0.0f) {
				s.streamSecs += a_delta;
			}
			const bool ending = state == RE::MagicCaster::State::kNone;
			// One SCST message per staff cast, whatever told us it was cast (Heart of Magic counts only the message and
			// skips the game's event for copies, 2026-11-18). Summons can skip the release states (2026-11-17 16:11): then
			// the game's event (absent under DMMF), an effect from this hand landing on the player, or the paid cost shows
			// the cast. Each sign belongs to this hand (Astra 2026-11-20): the game's event names no hand, so it does not
			// count while the other hand casts the same copy. Only the release states count for a plain hand cast.
			// Kept for the whole sample, not only while the other hand still casts (Astra 2026-11-20).
			// From the other hand's live state and slot, so the order the hands are watched in does not matter (Opus
			// 2026-11-20: a left instant cast before the left sample opened was counted for the right hand).
			const auto otherSide = Other(a_side);
			s.sharedCopy |= StateOf(a_player, Source(otherSide)) != RE::MagicCaster::State::kNone &&
			                a_player->GetActorRuntimeData().selectedSpells[SlotType(otherSide)] == s.item;
			const bool eventIsOurs = !s.sharedCopy;
			// An effect the engine gave no hand (casting source other / instant) counts for this hand unless the other
			// hand casts the same copy: the 8dd065d build counted every source, and summons may land that way (Opus).
			const bool handEffect = effect_redirect::CopyApplications(a_side) != s.effectsAtStart;
			const bool otherEffect = !s.sharedCopy && effect_redirect::CopyApplicationsWithoutHand() != s.unhandedEffectsAtStart;
			const core::ReleaseSignals signals{
				.releaseState = state == RE::MagicCaster::State::kUnk04 || state == RE::MagicCaster::State::kCasting,
				.castEvent = !s.plain && eventIsOurs && CastEventsOf(s.item) != s.castEventsAtStart,
				.effectApplied = !s.plain && (handEffect || otherEffect),
				.paidEnough = !s.plain && ending && s.cost >= 1.0f && s.deepestDrop >= s.cost * kPayShare,
				.otherHandCast = s.otherHandCast,
				.slotChanged = s.slotChanged,
			};
			if (!s.released && core::CastReleased(signals)) {
				s.released = true;
				s.releasedBy = signals.releaseState ? "release state" : signals.castEvent ? "cast event" : signals.effectApplied ? "effect on the player" : "magicka paid";
			}
			if (s.released && !s.plain && !s.announced) {
				s.announced = true;
				AnnounceStaffCast(s);
			}
			if (ending) {
				s.active = false;
				if (s.plain) {
					FinishHand(a_player, s);
				} else {
					FinishCopy(a_player, s);
				}
			}
		}
	}

	void OnFrame(RE::PlayerCharacter* a_player, float a_delta)
	{
		if (std::isfinite(a_delta) && a_delta > 0.0f) {
			g_clock += a_delta;
		}
		JudgeXp(a_player);
		for (const auto hand : kHands) {
			WatchHand(a_player, hand, a_delta);
		}
	}
}
