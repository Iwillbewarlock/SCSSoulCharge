#include "PCH.h"

#include "casting.h"

#include "conversion.h"
#include "core/charge_rules.h"
#include "keywords.h"
#include "pool.h"
#include "selftest.h"
#include "settings.h"
#include "watch.h"

#include <chrono>

namespace scssc::casting
{
	namespace
	{
		constexpr std::size_t   kCastSpellImmediateSlot = 0x01;
		constexpr std::uint32_t kCastingState = static_cast<std::uint32_t>(RE::MagicCaster::State::kCasting);  // 6, as SCS observed
		constexpr auto          kNoticeInterval = std::chrono::seconds(2);

		using CastFn = void (*)(RE::MagicCaster*, RE::MagicItem*, bool, RE::TESObjectREFR*, float, bool, float, RE::Actor*);
		REL::Relocation<CastFn>  g_originalCast;
		REL::Relocation<void (*)()> g_originalFrame;

		struct Channel
		{
			bool           running{ false };
			bool           left{ false };
			RE::SpellItem* spell{ nullptr };
			float          costPerSecond{ 0.0f };
		};
		Channel g_channel;  // SCS allows one concentration staff channel at a time (Casting.cpp:727)

		std::chrono::steady_clock::time_point g_lastNotice{};

		RE::ActorValue ChargeValue(bool a_left)
		{
			return a_left ? RE::ActorValue::kLeftItemCharge : RE::ActorValue::kRightItemCharge;
		}

		float ReadCharge(RE::PlayerCharacter* a_player, bool a_left)
		{
			auto* values = a_player->AsActorValueOwner();
			return values ? values->GetActorValue(ChargeValue(a_left)) : std::numeric_limits<float>::quiet_NaN();
		}

		std::array<float, 2> g_billed{};  // running total per hand (0 = left), read by the self-test watch

		// Positive = take charge, negative = give it back (refund of a rejected rune/summon).
		void Debit(RE::PlayerCharacter* a_player, bool a_left, float a_amount)
		{
			if (a_amount == 0.0f || !std::isfinite(a_amount)) {
				return;
			}
			if (auto* values = a_player->AsActorValueOwner()) {
				values->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, ChargeValue(a_left), -a_amount);
				g_billed[a_left ? 0 : 1] += a_amount;
			}
		}

		void NoticeEmpty()
		{
			const auto now = std::chrono::steady_clock::now();
			if (now - g_lastNotice >= kNoticeInterval) {
				g_lastNotice = now;
				RE::SendHUDMessage::ShowHUDMessage("Staff charge too low");
			}
		}

		float ChargeCost(RE::EnchantmentItem* a_enchantment, RE::PlayerCharacter* a_player)
		{
			// Which argument matches vanilla staff use is checked by T-C4; both are logged on every shot.
			return a_enchantment->CalculateMagickaCost(a_player);
		}

		bool SpellHotbarHoldsOther()
		{
			// Spell Hotbar 2 reserves kOther through this global (2 = kOther); SCS yields to it (Casting.cpp:711-717, :888-891).
			auto* data = RE::TESDataHandler::GetSingleton();
			static auto* global = data ? data->LookupForm<RE::TESGlobal>(0x0835, "SpellHotbar.esp") : nullptr;
			if (!global) {
				return false;
			}
			const float value = global->value;
			return !std::isfinite(value) || std::round(value) == static_cast<float>(RE::MagicSystem::CastingSource::kOther);
		}

		bool OwnsCast(const RE::MagicCaster* a_caster, const RE::MagicItem* a_spell)
		{
			return a_caster && a_spell && a_caster->currentSpell == a_spell && a_caster->state.underlying() != 0;
		}

		void LogFirstCall(const RE::MagicCaster* a_caster, const RE::MagicItem* a_spell)
		{
			static bool logged = false;
			if (!logged) {
				logged = true;
				logger::info("CastSpellImmediate hook first call: spell={:08X} source={} (identity check for this runtime)",
					a_spell ? a_spell->GetFormID() : 0u, a_caster ? static_cast<int>(a_caster->GetCastingSource()) : -1);
			}
		}

		void CastThunk(RE::MagicCaster* a_caster, RE::MagicItem* a_spell, bool a_noHitEffectArt, RE::TESObjectREFR* a_target,
			float a_effectiveness, bool a_hostileOnly, float a_magnitudeOverride, RE::Actor* a_blame)
		{
			LogFirstCall(a_caster, a_spell);
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!pool::IsPoolSpell(a_spell) || !player || !a_caster || a_caster->GetCasterAsActor() != player) {
				g_originalCast(a_caster, a_spell, a_noHitEffectArt, a_target, a_effectiveness, a_hostileOnly, a_magnitudeOverride, a_blame);
				return;
			}
			auto*      spell = static_cast<RE::SpellItem*>(a_spell);
			auto*      enchantment = pool::EnchantmentOf(spell);
			const auto hand = conversion::HandForSpell(player, spell);
			if (!enchantment || !hand) {
				logger::warn("CAST {:08X} refused: no held converted staff backs this charge spell", spell->GetFormID());
				NoticeEmpty();
				return;
			}
			const bool  left = *hand;
			const float charge = ReadCharge(player, left);
			const float cost = ChargeCost(enchantment, player);
			const bool  concentration = spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;

			if (concentration) {
				if (!core::AllowConcentrationStart(charge, cost)) {
					logger::info("CAST {} {:08X} concentration refused: charge={:.2f} cost/s={:.2f}", left ? "L" : "R", spell->GetFormID(), charge, cost);
					selftest::Report("T-C1", !OwnsCast(a_caster, spell),
						std::format("concentration blocked at charge={:.2f} cost/s={:.2f}; caster idle={}", charge, cost, !OwnsCast(a_caster, spell)));
					NoticeEmpty();
					return;
				}
				g_originalCast(a_caster, a_spell, a_noHitEffectArt, a_target, a_effectiveness, a_hostileOnly, a_magnitudeOverride, a_blame);
				if (OwnsCast(a_caster, spell)) {  // the engine may reject the start (SCS checks the same, Casting.cpp:785, :800-805)
					g_channel = { true, left, spell, cost };
					logger::info("CAST {} {:08X} concentration started: charge={:.2f} cost/s={:.2f}", left ? "L" : "R", spell->GetFormID(), charge, cost);
				}
				return;
			}

			if (!core::AllowFireAndForget(charge, cost)) {
				logger::info("CAST {} {:08X} refused: charge={:.2f} cost={:.2f}", left ? "L" : "R", spell->GetFormID(), charge, cost);
				selftest::Report("T-C1", !OwnsCast(a_caster, spell),
					std::format("shot blocked at charge={:.2f} cost={:.2f}; caster idle={}", charge, cost, !OwnsCast(a_caster, spell)));
				NoticeEmpty();
				return;
			}
			// Bill before the engine call: self bound-weapon spells unequip the staff inside it (SCS settles
			// first for the same reason, Casting.cpp:415-417), and the hand's charge would then be another item's.
			Debit(player, left, cost);
			const float afterDebit = ReadCharge(player, left);
			g_originalCast(a_caster, a_spell, a_noHitEffectArt, a_target, a_effectiveness, a_hostileOnly, a_magnitudeOverride, a_blame);
			const bool targetLocationFailed = spell->GetDelivery() == RE::MagicSystem::Delivery::kTargetLocation &&
			                                  a_caster->currentSpell == spell && a_caster->state.underlying() == kCastingState;
			const float refund = cost - core::FireAndForgetDebit(cost, targetLocationFailed);
			if (refund > 0.0f && conversion::HandForSpell(player, spell) == left) {
				Debit(player, left, -refund);  // rune or summon placement was rejected: the shot costs nothing
			}
			logger::info("CAST {} {:08X} '{}' fired: cost={:.2f} costNoActor={:.2f} enchRecordCost={} targetLocationFailed={} charge {:.2f} -> {:.2f}",
				left ? "L" : "R", spell->GetFormID(), spell->GetName(), cost, enchantment->CalculateMagickaCost(nullptr),
				enchantment->data.costOverride, targetLocationFailed, charge, afterDebit);
			selftest::Report("T-C1", std::fabs((charge - afterDebit) - cost) < 0.01f,
				std::format("shot billed {:.2f} expected {:.2f} (charge {:.2f} -> {:.2f}), refund {:.2f}", charge - afterDebit, cost, charge, afterDebit, refund));
		}

		void StopChannel(RE::PlayerCharacter* a_player, std::string_view a_reason, bool a_interrupt)
		{
			if (!g_channel.running) {
				return;
			}
			const auto saved = g_channel;
			g_channel = {};
			if (a_interrupt) {
				auto* other = a_player->GetMagicCaster(RE::MagicSystem::CastingSource::kOther);
				if (OwnsCast(other, saved.spell)) {
					other->InterruptCast(false);
				}
				// SCS keeps its dummy staff ENCH animating in the hand until release; stop only that ENCH (as SCS does, Casting.cpp:692-693).
				auto* hand = a_player->GetMagicCaster(saved.left ? RE::MagicSystem::CastingSource::kLeftHand : RE::MagicSystem::CastingSource::kRightHand);
				if (OwnsCast(hand, pool::SharedEnchantment())) {
					hand->InterruptCast(false);
				}
			}
			logger::info("CHANNEL {} {:08X} stopped: {}{}", saved.left ? "L" : "R", saved.spell->GetFormID(), a_reason,
				a_interrupt ? "" : " (caster not ours to interrupt)");
		}

		void UpdateChannel(RE::PlayerCharacter* a_player, float a_delta)
		{
			if (!g_channel.running) {
				return;
			}
			auto* other = a_player->GetMagicCaster(RE::MagicSystem::CastingSource::kOther);
			if (!OwnsCast(other, g_channel.spell)) {
				g_channel = {};  // ended natively (release, SCS stop, interruption)
				return;
			}
			if (SpellHotbarHoldsOther()) {
				StopChannel(a_player, "Spell Hotbar 2 reserved kOther", false);
				return;
			}
			if (conversion::HandForSpell(a_player, g_channel.spell) != g_channel.left) {
				StopChannel(a_player, "staff left the hand", true);  // never bill whatever the hand holds now
				return;
			}
			const float charge = ReadCharge(a_player, g_channel.left);
			const auto  tick = core::ConcentrationTick(charge, g_channel.costPerSecond, a_delta);
			Debit(a_player, g_channel.left, tick.debit);
			if (tick.interrupt) {
				StopChannel(a_player, std::format("charge {:.2f} left", charge - tick.debit), true);
			}
		}

		void RefreshOpenItemMenus()
		{
			auto* tasks = SKSE::GetTaskInterface();
			if (!tasks) {
				return;
			}
			tasks->AddUITask([] {
				auto* ui = RE::UI::GetSingleton();
				if (ui && ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME)) {
					if (auto menu = ui->GetMenu<RE::InventoryMenu>(); menu && menu->GetRuntimeData().itemList) {
						menu->GetRuntimeData().itemList->Update();
					}
				}
			});
		}

		void KeepNames(RE::PlayerCharacter* a_player)
		{
			if (!Settings::Get().keepStaffNames) {
				return;
			}
			bool anyRestored = false;
			for (const bool left : { true, false }) {
				auto* weapon = a_player->GetEquippedObject(left);
				if (!weapon || !keywords::IsConverted(weapon->GetFormID())) {
					continue;
				}
				const auto* original = keywords::OriginalName(weapon->GetFormID());
				auto*       named = weapon->As<RE::TESFullName>();
				bool        restored = false;
				if (original && named && named->fullName != original->c_str()) {
					named->fullName = original->c_str();  // SCS writes the spell name once per binding change (Naming.cpp:11)
					restored = true;
				}
				restored |= conversion::RestoreItemName(a_player, left);  // per-item names SCS overwrites (Naming.cpp:20-38)
				if (restored) {
					anyRestored = true;
					const char* now = named ? named->fullName.c_str() : nullptr;
					selftest::Report("T-NAME", original && now && *original == now,
						std::format("weapon={:08X} name='{}' original='{}'", weapon->GetFormID(), now ? now : "", original ? *original : ""));
				}
			}
			if (anyRestored) {
				RefreshOpenItemMenus();
			}
		}

		float FrameDelta()
		{
			static const auto address = REL::VariantID(523660, 410199, 0x30C3A08).address();  // same global SCS reads
			return *reinterpret_cast<const float*>(address);
		}

		void FrameThunk()
		{
			g_originalFrame();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || !pool::IsReady()) {
				return;
			}
			KeepNames(player);  // also while paused: SCS previews names in open menus
			conversion::GuardBareHands(player);  // equip events alone may miss a path (load, scripts)
			auto* ui = RE::UI::GetSingleton();
			if (ui && ui->GameIsPaused()) {
				return;
			}
			UpdateChannel(player, FrameDelta());
			watch::OnFrame(player);
		}

		std::size_t FrameCallOffset()
		{
			const auto version = REL::Module::get().version();
			if (version == REL::Version{ 1, 7, 104, 0 }) {
				return 0xC38;
			}
			if (version == REL::Version{ 1, 6, 1170, 0 }) {
				return 0xC26;
			}
			return 0x748;  // 1.5.97
		}
	}

	bool InstallCastHook()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_ActorMagicCaster[0] };
		const auto current = reinterpret_cast<const std::uintptr_t*>(vtable.address())[kCastSpellImmediateSlot];
		const auto base = REL::Module::get().base();
		const auto text = REL::Module::get().segment(REL::Segment::textx);
		const bool inGame = current >= base + text.offset() && current < base + text.offset() + text.size();
		logger::info("CastSpellImmediate slot target {:X} ({})", current, inGame ? "inside the game code" : "outside: another plugin hooked it first");
		g_originalCast = vtable.write_vfunc(kCastSpellImmediateSlot, CastThunk);
		logger::info("Hook installed: ActorMagicCaster::CastSpellImmediate (vtable slot 0x{:X})", kCastSpellImmediateSlot);
		return true;
	}

	bool InstallFrameHook()
	{
		const REL::Relocation<std::uintptr_t> mainUpdate{ REL::VariantID(35565, 36564, 0) };
		const auto site = mainUpdate.address() + FrameCallOffset();
		if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8) {
			logger::critical("Frame hook site {:X} is not a CALL (opcode {:02X}); frame work disabled", site,
				*reinterpret_cast<const std::uint8_t*>(site));
			return false;
		}
		auto& trampoline = SKSE::GetTrampoline();
		g_originalFrame = trampoline.write_call<5>(site, FrameThunk);
		logger::info("Hook installed: Main::Update frame call (+0x{:X})", FrameCallOffset());
		return true;
	}

	float TotalBilled(bool a_left)
	{
		return g_billed[a_left ? 0 : 1];
	}
}
