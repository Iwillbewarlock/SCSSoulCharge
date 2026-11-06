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
		constexpr double     kTolerance = 0.5;
		constexpr RE::FormID kTestStaffLocal = 0xC00;
		constexpr auto       kOwnPlugin = "SCSSoulCharge.esp"sv;
		// A native staff shot releases at state 4 and fires on 4 -> 0; a cancelled cast goes 0-1-2-0 and
		// is refunded by the engine (StaffCasting-spike.log:17-37 fired, :306-339 cancelled).
		constexpr std::uint32_t kReleasedState = 4;

		struct CastSession
		{
			bool                     active{ false };
			const RE::MagicItem*     spell{ nullptr };
			const RE::TESObjectWEAP* weapon{ nullptr };
			const RE::ExtraDataList* list{ nullptr };
			double                   startCharge{ 0.0 };
			double                   startBilled{ 0.0 };
			double                   worstResidual{ 0.0 };  // largest |engine drain| seen on any frame of the cast
			bool                     released{ false };
		};

		struct HeldStaff
		{
			const RE::TESObjectWEAP* weapon{ nullptr };
			const RE::ExtraDataList* list{ nullptr };
			double                   lastCharge{ 0.0 };
			double                   billedAtLast{ 0.0 };  // our billing total when lastCharge was read
		};

		std::array<CastSession, 2> g_sessions{};  // 0 = left
		std::array<HeldStaff, 2>   g_held{};
		bool                       g_c2bReported = false;
		RE::TESObjectWEAP*         g_testStaff = nullptr;

		std::size_t Index(bool a_left) { return a_left ? 0 : 1; }

		double ReadCharge(RE::PlayerCharacter* a_player, bool a_left)
		{
			auto* values = a_player->AsActorValueOwner();
			return values ? values->GetActorValue(a_left ? RE::ActorValue::kLeftItemCharge : RE::ActorValue::kRightItemCharge) : 0.0;
		}

		// The worn item is one we converted: its own list carries SCS's shared enchantment.
		bool WornIsConverted(const RE::TESObjectWEAP* a_staff, const RE::ExtraDataList* a_list)
		{
			const auto* shared = pool::SharedEnchantment();
			const auto* extra = a_list ? a_list->GetByType<RE::ExtraEnchantment>() : nullptr;
			return a_staff && keywords::IsConverted(a_staff->GetFormID()) && extra && extra->enchantment == shared;
		}

		bool InPlayerInventory(RE::PlayerCharacter* a_player, const RE::TESObjectWEAP* a_base, const RE::ExtraDataList* a_list)
		{
			auto* changes = a_player->GetInventoryChanges();
			if (!changes || !changes->entryList || !a_list) {
				return false;
			}
			for (auto* entry : *changes->entryList) {
				if (entry && entry->object == a_base && entry->extraLists) {
					for (auto* list : *entry->extraLists) {
						if (list == a_list) {
							return true;
						}
					}
				}
			}
			return false;
		}

		double Residual(RE::PlayerCharacter* a_player, bool a_left, const CastSession& a_session)
		{
			const double billed = casting::TotalBilled(a_left) - a_session.startBilled;
			return a_session.startCharge - ReadCharge(a_player, a_left) - billed;
		}

		void FinishSession(RE::PlayerCharacter* a_player, bool a_left, const CastSession& a_session)
		{
			const double engineDrain = Residual(a_player, a_left, a_session);
			if (WornIsConverted(a_session.weapon, a_session.list) && a_session.spell == pool::SharedEnchantment()) {
				// Any frame counts: a cancelled cast may drain and refund on the way (cancel path above).
				const double worst = std::max(std::fabs(engineDrain), a_session.worstResidual);
				selftest::Report("T-C5", worst < kTolerance,
					std::format("hand={} weapon={:08X} released={} engine drain at end {:.2f}, worst frame {:.2f} (expect 0)", a_left ? "L" : "R",
						a_session.weapon->GetFormID(), a_session.released, engineDrain, a_session.worstResidual));
				return;
			}
			if (g_testStaff && a_session.weapon == g_testStaff && a_session.spell == g_testStaff->formEnchanting) {
				if (!a_session.released) {
					selftest::Skip("T-C4", std::format("cast cancelled before release (drain {:.2f}); fire one full shot", engineDrain));
					return;
				}
				auto*        enchantment = g_testStaff->formEnchanting;
				const double withPlayer = enchantment->CalculateMagickaCost(a_player);
				const double noActor = enchantment->CalculateMagickaCost(nullptr);
				const bool   playerMatches = std::fabs(engineDrain - withPlayer) < kTolerance;
				const bool   noActorMatches = std::fabs(engineDrain - noActor) < kTolerance;
				selftest::Report("T-C4", playerMatches || noActorMatches,
					std::format("vanilla shot drained {:.2f}; CalculateMagickaCost(player)={:.2f} match={} (nullptr)={:.2f} match={} recordCost={}",
						engineDrain, withPlayer, playerMatches, noActor, noActorMatches, enchantment->data.costOverride));
			}
		}

		void WatchHand(RE::PlayerCharacter* a_player, bool a_left)
		{
			auto*       weapon = a_player->GetEquippedObject(a_left);
			auto*       staff = weapon ? weapon->As<RE::TESObjectWEAP>() : nullptr;
			const auto* list = conversion::WornList(a_player, a_left);
			auto&       held = g_held[Index(a_left)];
			if (WornIsConverted(staff, list)) {
				held = { staff, list, ReadCharge(a_player, a_left), casting::TotalBilled(a_left) };
			}

			auto*      caster = a_player->GetMagicCaster(a_left ? RE::MagicSystem::CastingSource::kLeftHand : RE::MagicSystem::CastingSource::kRightHand);
			const auto state = caster ? caster->state.underlying() : 0u;
			const bool active = caster && caster->currentSpell && state != 0;
			auto&      session = g_sessions[Index(a_left)];

			if (session.active && active && (caster->currentSpell != session.spell || staff != session.weapon || list != session.list)) {
				session = {};  // spell or item changed mid-cast: start and end would describe different things
				return;
			}
			if (active && !session.active) {
				session = { true, caster->currentSpell, staff, list, ReadCharge(a_player, a_left), casting::TotalBilled(a_left), 0.0, false };
				if (!g_c2bReported && WornIsConverted(staff, list)) {
					g_c2bReported = true;
					const auto* shared = pool::SharedEnchantment();
					selftest::Report("T-C2b", caster->currentSpell == shared,
						std::format("hand={} weapon={:08X} castingEnchantment={:08X} shared={:08X}", a_left ? "L" : "R", staff->GetFormID(),
							caster->currentSpell->GetFormID(), shared ? shared->GetFormID() : 0u));
				}
			} else if (active && session.active) {
				session.released |= state >= kReleasedState;
				session.worstResidual = std::max(session.worstResidual, std::fabs(Residual(a_player, a_left, session)));
			} else if (!active && session.active) {
				const auto finished = session;
				session = {};
				if (staff == finished.weapon && list == finished.list) {
					FinishSession(a_player, a_left, finished);
				}
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

	void Reset()
	{
		g_sessions = {};
		g_held = {};
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
			if (!InPlayerInventory(a_player, a_base, snapshot.list)) {
				selftest::Skip("T-C3", std::format("weapon={:08X}: item left the inventory before the check", a_base->GetFormID()));
				continue;
			}
			// Billing after the last observed frame (e.g. a bound-weapon shot that unequips the staff) still counts.
			const double expected = snapshot.lastCharge - (casting::TotalBilled(left) - snapshot.billedAtLast);
			const auto*  charge = snapshot.list->GetByType<RE::ExtraCharge>();
			const double maxCharge = a_base->amountofEnchantment;
			const bool   pass = charge ? std::fabs(charge->charge - expected) < kTolerance :
			                             expected >= maxCharge - kTolerance;  // no ExtraCharge = full
			selftest::Report("T-C3", pass,
				std::format("weapon={:08X} expectedCharge={:.2f} extraCharge={} max={:.0f}", a_base->GetFormID(), expected,
					charge ? std::format("{:.2f}", charge->charge) : "none", maxCharge));
		}
	}
}
