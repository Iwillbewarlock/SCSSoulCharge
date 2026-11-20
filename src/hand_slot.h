#pragma once

#include "hand.h"

// The player's hand slots (selectedSpells[kRightHand] / [kLeftHand]). Main thread only.
// Written the way the engine's own setter (38826) does it: preload the new item (11303), release the old one
// (11306), then write. The engine attaches a casting art only for a preloaded slot item (34185 -> 11304).
// One copy may sit in both slots at once (the same spell on two staves): each slot holds its own reference.

namespace scssc::hand_slot
{
	// Finds the preload functions. Only on 1.6.1170, the build whose code was read; false elsewhere.
	bool Install();

	RE::MagicItem* Current(RE::PlayerCharacter* a_player, Hand a_hand);

	// Preload new, release old, write. Nothing happens when a_item is already there.
	void Write(RE::PlayerCharacter* a_player, Hand a_hand, RE::MagicItem* a_item);

	// Counts Write calls that changed this hand's slot. The tip art refreshes after each, even when the item is the
	// one it last refreshed for (the engine may have put another item in between).
	std::uint32_t Writes(Hand a_hand);

	// Writes without touching the preload counts: for the save swap (the copy keeps its reference while the save is
	// written, and gets the slot back without a new one).
	void WriteRaw(RE::PlayerCharacter* a_player, Hand a_hand, RE::MagicItem* a_item);

	// Moves one preload reference from a_from to a_to (a save swap that is not undone).
	void MoveReference(RE::MagicItem* a_from, RE::MagicItem* a_to);
}
