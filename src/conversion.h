#pragma once

// Turns the player's converted staff items into SCS staves and back.
//  - Equip: the worn item gets SCS's shared enchantment as ExtraEnchantment (max charge = base EAMT),
//    and the player learns the charge spell for the staff's own enchantment.
//  - Unequip: the charge spell is forgotten unless the other hand still holds a staff using it.
//  - Leaving the player's inventory (drop, sell, give, steal from): the item gets its own enchantment back.
// Items the player enchanted themselves (an ExtraEnchantment that is not SCS's) are left alone.

namespace scssc::conversion
{
	void Install();

	// Co-save / load lifecycle.
	void SetBlocked(bool a_blocked, std::string_view a_reason);
	[[nodiscard]] bool IsBlocked();
	// Events are ignored until the save is settled (Revert -> false, kPostLoadGame/kNewGame -> true).
	void SetSettled(bool a_settled);
	void ReconcilePlayer();  // after load or new game: items and spells match what the player holds

	// The hand (true = left) whose held converted staff backs this charge spell.
	[[nodiscard]] std::optional<bool> HandForSpell(RE::PlayerCharacter* a_player, const RE::SpellItem* a_spell);

	// Unequips charge spells found in a bare hand. Called from equip events, after loads and every frame.
	void GuardBareHands(RE::PlayerCharacter* a_player);

	// Puts back a converted item's own display name if SCS replaced it. True when something changed.
	bool RestoreItemName(RE::PlayerCharacter* a_player, bool a_left);
}
