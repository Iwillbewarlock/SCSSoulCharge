#include "hand_slot.h"

#include "selftest.h"

#include <chrono>

namespace scssc::hand_slot
{
	namespace
	{
		// 11303 bumps preloadCount (+0x80) and queues the item's models into preloadedItem (+0x88); 11306 drops the
		// count and frees the queued file at zero (decrypted 1.6.1170 code, 2026-11-16).
		using PreloadFn = void (*)(RE::MagicItem*);
		using ReleaseFn = void (*)(RE::MagicItem*, bool);
		PreloadFn     g_preload = nullptr;
		ReleaseFn     g_release = nullptr;
		std::uint32_t g_writes = 0;  // main thread only

		// More than this many slot changes within one second means something rewrites the slot against us.
		constexpr std::uint32_t kFightWrites = 10;
		std::uint64_t           g_windowStartMs = 0;
		std::uint32_t           g_windowWrites = 0;
		bool                    g_fightReported = false;

		void CountForFight()
		{
			const auto now = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
			if (now - g_windowStartMs > 1000) {
				g_windowStartMs = now;
				g_windowWrites = 0;
			}
			if (++g_windowWrites > kFightWrites && !g_fightReported) {
				g_fightReported = true;
				selftest::Report("T-DRAW", false, std::format("{} hand slot writes within one second: something else keeps changing the slot", g_windowWrites));
			}
		}

		RE::MagicItem*& Slot(RE::PlayerCharacter* a_player)
		{
			return a_player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand];
		}
	}

	bool Install()
	{
		if (REL::Module::get().version() != REL::Version{ 1, 6, 1170, 0 }) {
			logger::critical("hand slot: the preload addresses are known for 1.6.1170 only; the mod stays off");
			return false;
		}
		g_preload = reinterpret_cast<PreloadFn>(REL::ID(11303).address());
		g_release = reinterpret_cast<ReleaseFn>(REL::ID(11306).address());
		logger::info("hand slot: preload 11303 at {:X}, release 11306 at {:X}", reinterpret_cast<std::uintptr_t>(g_preload),
			reinterpret_cast<std::uintptr_t>(g_release));
		return true;
	}

	RE::MagicItem* Current(RE::PlayerCharacter* a_player)
	{
		return a_player ? Slot(a_player) : nullptr;
	}

	void Write(RE::PlayerCharacter* a_player, RE::MagicItem* a_item)
	{
		auto& slot = Slot(a_player);
		if (slot == a_item) {
			return;
		}
		MoveReference(slot, a_item);
		logger::info("hand slot: {:08X} -> {:08X}", slot ? slot->GetFormID() : 0u, a_item ? a_item->GetFormID() : 0u);
		slot = a_item;
		++g_writes;
		CountForFight();
	}

	std::uint32_t Writes()
	{
		return g_writes;
	}

	void WriteRaw(RE::PlayerCharacter* a_player, RE::MagicItem* a_item)
	{
		Slot(a_player) = a_item;
	}

	void MoveReference(RE::MagicItem* a_from, RE::MagicItem* a_to)
	{
		if (a_from == a_to || !g_preload || !g_release) {
			return;
		}
		if (a_to) {
			g_preload(a_to);
		}
		if (a_from) {
			g_release(a_from, true);
		}
	}
}
