#include "hand_slot.h"

#include "runtime_check.h"
#include "selftest.h"

#include <array>

#include <chrono>

namespace scssc::hand_slot
{
	namespace
	{
		// 11303 bumps preloadCount (+0x80) and queues the item's models into preloadedItem (+0x88); 11306 drops the
		// count and frees the queued file at zero (decrypted 1.6.1170 code, 2026-11-16; same in the 1.7.104 executable).
		using PreloadFn = void (*)(RE::MagicItem*);
		using ReleaseFn = void (*)(RE::MagicItem*, bool);
		PreloadFn     g_preload = nullptr;
		ReleaseFn     g_release = nullptr;
		std::array<std::uint32_t, 2> g_writes{};  // main thread only, per hand

		// More than this many slot changes within one second means something rewrites the slot against us.
		constexpr std::uint32_t kFightWrites = 10;
		struct FightWindow
		{
			std::uint64_t startMs{ 0 };
			std::uint32_t writes{ 0 };
			bool          reported{ false };
		};
		std::array<FightWindow, 2> g_fight{};

		void CountForFight(Hand a_hand)
		{
			auto&      w = g_fight[Index(a_hand)];
			const auto now = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
			if (now - w.startMs > 1000) {
				w.startMs = now;
				w.writes = 0;
			}
			if (++w.writes > kFightWrites && !w.reported) {
				w.reported = true;
				selftest::Report("T-DRAW", false, std::format("{} {} hand slot writes within one second: something else keeps changing the slot", w.writes, Name(a_hand)));
			}
		}

		RE::MagicItem*& Slot(RE::PlayerCharacter* a_player, Hand a_hand)
		{
			return a_player->GetActorRuntimeData().selectedSpells[SlotType(a_hand)];
		}
	}

	bool Install()
	{
		const auto preload = runtime_check::PreloadItem();
		const auto release = runtime_check::ReleaseItem();
		if (!preload || !release) {
			logger::critical("hand slot: the preload functions did not pass their start-up check; the mod stays off");
			return false;
		}
		g_preload = reinterpret_cast<PreloadFn>(preload);
		g_release = reinterpret_cast<ReleaseFn>(release);
		return true;
	}

	RE::MagicItem* Current(RE::PlayerCharacter* a_player, Hand a_hand)
	{
		return a_player ? Slot(a_player, a_hand) : nullptr;
	}

	void Write(RE::PlayerCharacter* a_player, Hand a_hand, RE::MagicItem* a_item)
	{
		auto& slot = Slot(a_player, a_hand);
		if (slot == a_item) {
			return;
		}
		MoveReference(slot, a_item);
		logger::debug("hand slot {}: {:08X} -> {:08X}", Name(a_hand), slot ? slot->GetFormID() : 0u, a_item ? a_item->GetFormID() : 0u);
		slot = a_item;
		++g_writes[Index(a_hand)];
		CountForFight(a_hand);
	}

	std::uint32_t Writes(Hand a_hand)
	{
		return g_writes[Index(a_hand)];
	}

	void WriteRaw(RE::PlayerCharacter* a_player, Hand a_hand, RE::MagicItem* a_item)
	{
		Slot(a_player, a_hand) = a_item;
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
