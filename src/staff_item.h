#pragma once

#include "core/bindings.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

// The staff in the player's right hand as one inventory item: its record, its worn extra-data list and the engine's
// unique id that tells two staves of the same kind apart (design 7).

namespace scssc::staff_item
{
	// Looks up SCS's staff keyword once data is loaded; staves with it are never touched (design 11).
	void OnDataLoaded();

	// A staff this mod may bind: a staff, without SCS's keyword, not in the INI exclude list.
	bool IsTarget(const RE::TESObjectWEAP* a_weapon);

	struct Held
	{
		RE::TESObjectWEAP*  staff{ nullptr };
		RE::ExtraDataList*  list{ nullptr };   // the worn list of exactly this item
		std::int32_t        count{ 0 };        // items that share this list (must be 1 to bind)
	};

	// The target staff in the right hand, or empty. Never cached: lists can be rebuilt by the engine.
	std::optional<Held> RightHand(RE::PlayerCharacter* a_player);

	// The same for the left hand (its list is marked kWornLeft).
	std::optional<Held> LeftHand(RE::PlayerCharacter* a_player);

	// A staff item of record a_staff in the player's inventory that has no unique id and shows a_name, or empty.
	std::optional<Held> FindWithoutId(RE::PlayerCharacter* a_player, RE::FormID a_staff, std::string_view a_name);

	// Calls a_fn(held, key) for every staff item in the player's inventory that has a unique id.
	void ForEachWithId(RE::PlayerCharacter* a_player, const std::function<void(const Held&, const core::StaffKey&)>& a_fn);

	// The item's key, giving it a unique id first when a_assign is set (the engine's own path, which sends
	// TESUniqueIDChangeEvent). Empty when the item has no id and none could be given.
	std::optional<core::StaffKey> KeyOf(RE::PlayerCharacter* a_player, const Held& a_held, bool a_assign);

	// The name the inventory shows now.
	std::string DisplayName(const Held& a_held);

	// Whether the item already has its own name data (before we write ours).
	bool HasOwnName(const Held& a_held);

	// SetOverrideName writes nothing when the name data belongs to a quest or message (ExtraDataList.cpp:392).
	bool NameLocked(const Held& a_held);

	// Writes a_name and reads it back. False when the item shows another name afterwards.
	bool WriteName(const Held& a_held, const std::string& a_name);

	// Gives the name back as a binding ends (design 7-1). Returns what was done, for the log.
	const char* RestoreName(const Held& a_held, const core::NameState& a_state);

	// A guess at what the engine puts in the hand slot for this staff: the record's enchantment, else the item's
	// (the 11-06 T-C2 tests saw the engine prefer the record). staff_binding remembers the real slot value when it can.
	RE::MagicItem* OwnEnchantment(const Held& a_held);
}
