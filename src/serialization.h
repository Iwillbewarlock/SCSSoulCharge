#pragma once

// SKSE co-save ('SCSC' / 'BIND'): which staff carries which spell, and the names to give back (core::BindingTable).
// Revert (new game, before any load): empty table. Older test-build records (POOL, BLCK, COLD) are skipped.

namespace scssc::serialization
{
	// Always registered, even when the mod stays off, so the saved bindings pass through unchanged.
	void Install();

	// True once every hook is in place: bindings are read into the table and written from it.
	void SetActive(bool a_active);
}
