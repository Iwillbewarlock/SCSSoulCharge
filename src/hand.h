#pragma once

#include <array>
#include <cstdint>

// Which of the player's hands a staff is in. Both hands are managed the same way (user decision 2026-11-20).

namespace scssc
{
	enum class Hand : std::uint8_t
	{
		kRight = 0,
		kLeft = 1,
	};

	inline constexpr std::array<Hand, 2> kHands{ Hand::kRight, Hand::kLeft };

	constexpr std::size_t Index(Hand a_hand) { return static_cast<std::size_t>(a_hand); }
	constexpr Hand        Other(Hand a_hand) { return a_hand == Hand::kRight ? Hand::kLeft : Hand::kRight; }
	constexpr const char* Name(Hand a_hand) { return a_hand == Hand::kRight ? "right" : "left"; }

	inline RE::MagicSystem::CastingSource Source(Hand a_hand)
	{
		return a_hand == Hand::kRight ? RE::MagicSystem::CastingSource::kRightHand : RE::MagicSystem::CastingSource::kLeftHand;
	}

	// The index into Actor::selectedSpells (Actor::SlotTypes is a struct around an unnamed enum).
	inline std::uint32_t SlotType(Hand a_hand)
	{
		return a_hand == Hand::kRight ? RE::Actor::SlotTypes::kRightHand : RE::Actor::SlotTypes::kLeftHand;
	}

	inline RE::ActorValue ChargeValue(Hand a_hand)
	{
		return a_hand == Hand::kRight ? RE::ActorValue::kRightItemCharge : RE::ActorValue::kLeftItemCharge;
	}

	inline RE::ActorMagicCaster* Caster(RE::PlayerCharacter* a_player, Hand a_hand)
	{
		auto* caster = a_player ? a_player->GetMagicCaster(Source(a_hand)) : nullptr;
		return caster ? static_cast<RE::ActorMagicCaster*>(caster) : nullptr;
	}
}
