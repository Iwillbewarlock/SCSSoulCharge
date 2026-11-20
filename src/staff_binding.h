#pragma once

#include "core/bindings.h"
#include "hand.h"

#include <array>

// Picking a spell for a held staff from the magic menu, and keeping each hand's slot on its staff's spell copy
// (design 3, 5, 7, 8; both hands since 2026-11-20). All engine changes run on the main thread (main_queue, frame hook).

namespace scssc::staff_binding
{
	// The magic menu's spell equip call (37952/38908 +0xD7), chained after SCS's own hook: run on the first
	// kPostLoadGame/kNewGame, after SCS hooked the same call at its kDataLoaded. Also registers the event sinks.
	// When the call site is not a plain CALL, bindings stay saved but are not applied (no way to change them).
	void InstallMenuHook();

	// Whether a target staff is in each hand (index: Index(Hand)).
	using HandsHeld = std::array<bool, 2>;

	// Main thread, every unpaused frame: for each hand, put the bound copy in its slot, or the staff's own enchantment
	// back.
	HandsHeld OnFrame(RE::PlayerCharacter* a_player);

	// The same for one hand (after a pick). Returns whether a target staff is in that hand.
	bool SyncHand(RE::PlayerCharacter* a_player, Hand a_hand);

	// kSaveGame: each slot holds its staff's own enchantment while the save is written (T-SAVE), put back after.
	// a_mainThread: the frame hook's thread id (0 = not seen yet), for T-THREAD.
	void OnSave(std::uint32_t a_mainThread);

	// Load and new game: forget per-session state. The table itself is replaced by serialization.
	void OnReset();

	// The co-save was read with this many bindings (arms T-SAVE-REBIND).
	void OnLoaded(std::size_t a_bindings);

	// kPostLoadGame / kNewGame, after InstallMenuHook: the one-time notice when the menu hook failed, and the
	// [Uninstall] bRestoreNames run (queued for the main thread).
	void OnPostLoad();

	// The table for the co-save, with pending unique id changes applied first.
	core::BindingTable& TableForSave();

	core::BindingTable& Table();

	// Bindings are applied (false after the menu hook failed, or while the uninstall option runs). Any thread.
	bool Applying();

	// The [Uninstall] run, at once, for kPostLoadGame while the mod is off (a hook failed on the supported runtime).
	void RunUninstallNow();

	// The bound spell of the staff in that hand as of the last frame, or null (none, or not known right now).
	RE::SpellItem* ActiveSpell(Hand a_hand);
}
