#pragma once

// The engine's cost-resource lookup (33817/34609, the place Immersive Staves Tweaks hooks): our staff copies pay
// with magicka instead of the staff's charge. Decided by "is it one of our copies", not by the current binding, so a
// cast still running when the binding changes keeps paying with magicka (design 6). Proven 7/7 (T-PAY 2026-11-17).

// Also MagicItem::CalculateCost: a copy costs what its spell costs the caster (perks, the spell's own skill).

namespace scssc::resource_hook
{
	// False when the lookup could not be hooked; the mod must then stay off (copies would drain the staff).
	bool Install();
}
