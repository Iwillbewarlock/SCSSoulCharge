#pragma once

// SKSE co-save: the charge-spell slot table ('SCSC' / 'POOL').
// Revert (new game, before any load): empty table, empty spells.
// Load OK: resolve FormIDs, refill spells, reconcile the player's hands.
// Load failed or table missing while the player knows charge spells: conversion blocked for this save,
// so no slot can be handed to another enchantment while SCS still remembers the old one.

namespace scssc::serialization
{
	void Install();
	void OnPostLoad(bool a_loadSucceeded);  // kPostLoadGame
	void OnNewGame();                       // kNewGame
}
