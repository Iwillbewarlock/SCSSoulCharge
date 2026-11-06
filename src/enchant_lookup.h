#pragma once

// In-game test (2026-11-06): the engine's item-enchantment lookup InventoryEntryData::GetEnchantment
// (SE 15788 / AE 16026) returns the staff record's own enchantment even when the worn item carries
// SCS's shared enchantment as ExtraEnchantment, so the hand cast the staff's own spell and SCS gave up
// ("native cast never started"). This hook makes the lookup return the shared enchantment for a worn
// item we converted, and changes nothing for any other item.

namespace scssc::enchant_lookup
{
	bool Install();
}
