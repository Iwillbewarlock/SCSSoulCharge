#pragma once

#include "core/bindings.h"

// Picking a spell for the held staff from the magic menu, and keeping the hand slot on the staff's spell copy
// (design 3, 5, 7, 8). All engine changes run on the main thread (main_queue, frame hook).

namespace scssc::staff_binding
{
	// The magic menu's spell equip call (37952/38908 +0xD7), chained after SCS's own hook: run on the first
	// kPostLoadGame/kNewGame, after SCS hooked the same call at its kDataLoaded. Also registers the event sinks.
	void InstallMenuHook();

	// Main thread, every unpaused frame: put the bound copy in the hand slot, or the staff's own enchantment back.
	// Returns whether a target staff is in the right hand.
	bool OnFrame(RE::PlayerCharacter* a_player);

	// kSaveGame: the slot holds the staff's own enchantment while the save is written (T-SAVE), put back after.
	// a_mainThread: the frame hook's thread id (0 = not seen yet), for T-THREAD.
	void OnSave(std::uint32_t a_mainThread);

	// Load and new game: forget per-session state. The table itself is replaced by serialization.
	void OnReset();

	// The co-save was read with this many bindings (arms T-SAVE-REBIND).
	void OnLoaded(std::size_t a_bindings);

	// The table for the co-save, with pending unique id changes applied first.
	core::BindingTable& TableForSave();

	core::BindingTable& Table();

	// The bound spell of the staff in the right hand as of the last frame, or null (none, or not known right now).
	RE::SpellItem* ActiveSpell();
}
