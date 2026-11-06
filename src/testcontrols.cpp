#include "PCH.h"

#include "testcontrols.h"

#include "conversion.h"
#include "keywords.h"
#include "pool.h"
#include "settings.h"

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
					if (!button || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard || !button->IsDown()) {
						continue;
					}
					const auto key = button->GetIDCode();
					// Input sinks may run off the main thread: hand the work to the game thread.
					if (auto* tasks = SKSE::GetTaskInterface()) {
						if (key == kKeyBind) {
							tasks->AddTask([] { BindChargeSpell(false); });
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
	}

	void OnFrame(float a_delta)
	{
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
