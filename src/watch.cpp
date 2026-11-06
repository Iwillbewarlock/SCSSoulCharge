#include "PCH.h"

#include "watch.h"

#include "casting.h"
#include "conversion.h"
#include "keywords.h"
#include "pool.h"
#include "selftest.h"
#include "settings.h"

#include <Windows.h>

namespace scssc::watch
{
	namespace
	{
		constexpr float      kTolerance = 0.5f;
		constexpr RE::FormID kTestStaffLocal = 0xC00;
		constexpr auto       kOwnPlugin = "SCSSoulCharge.esp"sv;

		struct CastSession
		{
			bool                 active{ false };
			const RE::MagicItem* spell{ nullptr };
			const RE::TESObjectWEAP* weapon{ nullptr };
			float                startCharge{ 0.0f };
			float                startBilled{ 0.0f };
		};

		struct HeldStaff
		{
			const RE::TESObjectWEAP*  weapon{ nullptr };
			const RE::ExtraDataList*  list{ nullptr };
			float                     lastCharge{ 0.0f };
		};

		std::array<CastSession, 2> g_sessions{};  // 0 = left
		std::array<HeldStaff, 2>   g_held{};
		bool                       g_c2bReported = false;
		RE::TESObjectWEAP*         g_testStaff = nullptr;

		std::size_t Index(bool a_left) { return a_left ? 0 : 1; }

		float ReadCharge(RE::PlayerCharacter* a_player, bool a_left)
		{
			auto* values = a_player->AsActorValueOwner();
			return values ? values->GetActorValue(a_left ? RE::ActorValue::kLeftItemCharge : RE::ActorValue::kRightItemCharge) : 0.0f;
		}

		bool InPlayerInventory(RE::PlayerCharacter* a_player, const RE::ExtraDataList* a_list)
		{
			auto* changes = a_player->GetInventoryChanges();
			if (!changes || !changes->entryList || !a_list) {
				return false;
			}
			for (auto* entry : *changes->entryList) {
				if (entry && entry->extraLists) {
					for (auto* list : *entry->extraLists) {
						if (list == a_list) {
							return true;
						}
					}
				}
			}
			return false;
		}

		void FinishSession(RE::PlayerCharacter* a_player, bool a_left, const CastSession& a_session)
		{
			const float endCharge = ReadCharge(a_player, a_left);
			const float billed = casting::TotalBilled(a_left) - a_session.startBilled;
			const float engineDrain = a_session.startCharge - endCharge - billed;
			const auto* shared = pool::SharedEnchantment();
			if (a_session.spell == shared && a_session.weapon && keywords::IsConverted(a_session.weapon->GetFormID())) {
				selftest::Report("T-C5", std::fabs(engineDrain) < kTolerance,
					std::format("hand={} weapon={:08X} charge {:.2f} -> {:.2f}, ours {:.2f}, engine {:.2f} (expect 0)", a_left ? "L" : "R",
						a_session.weapon->GetFormID(), a_session.startCharge, endCharge, billed, engineDrain));
				return;
			}
			if (g_testStaff && a_session.weapon == g_testStaff && a_session.spell == g_testStaff->formEnchanting) {
				auto*       enchantment = g_testStaff->formEnchanting;
				const float withPlayer = enchantment->CalculateMagickaCost(a_player);
				const float noActor = enchantment->CalculateMagickaCost(nullptr);
				const bool  playerMatches = std::fabs(engineDrain - withPlayer) < kTolerance;
				const bool  noActorMatches = std::fabs(engineDrain - noActor) < kTolerance;
				selftest::Report("T-C4", playerMatches || noActorMatches,
					std::format("vanilla shot drained {:.2f}; CalculateMagickaCost(player)={:.2f} match={} (nullptr)={:.2f} match={} recordCost={}",
						engineDrain, withPlayer, playerMatches, noActor, noActorMatches, enchantment->data.costOverride));
			}
		}

		void WatchHand(RE::PlayerCharacter* a_player, bool a_left)
		{
			auto* weapon = a_player->GetEquippedObject(a_left);
			auto* staff = weapon ? weapon->As<RE::TESObjectWEAP>() : nullptr;
			auto& held = g_held[Index(a_left)];
			if (staff && keywords::IsConverted(staff->GetFormID())) {
				held = { staff, conversion::WornList(a_player, a_left), ReadCharge(a_player, a_left) };
			}

			auto*       caster = a_player->GetMagicCaster(a_left ? RE::MagicSystem::CastingSource::kLeftHand : RE::MagicSystem::CastingSource::kRightHand);
			const bool  active = caster && caster->currentSpell && caster->state.underlying() != 0;
			auto&       session = g_sessions[Index(a_left)];
			if (active && !session.active) {
				session = { true, caster->currentSpell, staff, ReadCharge(a_player, a_left), casting::TotalBilled(a_left) };
				if (!g_c2bReported && staff && keywords::IsConverted(staff->GetFormID())) {
					g_c2bReported = true;
					const auto* shared = pool::SharedEnchantment();
					selftest::Report("T-C2b", caster->currentSpell == shared,
						std::format("hand={} weapon={:08X} castingEnchantment={:08X} shared={:08X}", a_left ? "L" : "R", staff->GetFormID(),
							caster->currentSpell->GetFormID(), shared ? shared->GetFormID() : 0u));
				}
			} else if (!active && session.active) {
				const auto finished = session;
				session = {};
				FinishSession(a_player, a_left, finished);
			}
		}
	}

	void OnDataLoaded()
	{
		if (::GetModuleHandleW(L"StaffCasting.dll")) {
			logger::critical("StaffCasting.dll is loaded: it redirects spell equips and builds its own staff spells, which conflicts with SCSSoulCharge");
			RE::SendHUDMessage::ShowHUDMessage("SCSSoulCharge: StaffCasting is installed too; use only one of them");
		}
		if (auto* data = RE::TESDataHandler::GetSingleton()) {
			g_testStaff = data->LookupForm<RE::TESObjectWEAP>(kTestStaffLocal, kOwnPlugin);
		}
	}

	void OnFrame(RE::PlayerCharacter* a_player)
	{
		if (!Settings::Get().selfTest) {
			return;
		}
		WatchHand(a_player, true);
		WatchHand(a_player, false);
	}

	void OnUnequip(RE::PlayerCharacter* a_player, const RE::TESObjectWEAP* a_base)
	{
		if (!Settings::Get().selfTest) {
			return;
		}
		for (const bool left : { true, false }) {
			auto& held = g_held[Index(left)];
			if (held.weapon != a_base || a_player->GetEquippedObject(left) == a_base) {
				continue;
			}
			const auto snapshot = held;
			held = {};
			if (!InPlayerInventory(a_player, snapshot.list)) {
				selftest::Report("T-C3", false, std::format("weapon={:08X}: item left the inventory before the check; not judged", a_base->GetFormID()));
				continue;
			}
			const auto* charge = snapshot.list->GetByType<RE::ExtraCharge>();
			const float maxCharge = static_cast<float>(a_base->amountofEnchantment);
			const bool  pass = charge ? std::fabs(charge->charge - snapshot.lastCharge) < kTolerance :
			                            snapshot.lastCharge >= maxCharge - kTolerance;  // no ExtraCharge = full
			selftest::Report("T-C3", pass,
				std::format("weapon={:08X} chargeInHand={:.2f} extraCharge={} max={:.0f}", a_base->GetFormID(), snapshot.lastCharge,
					charge ? std::format("{:.2f}", charge->charge) : "none", maxCharge));
		}
	}
}
