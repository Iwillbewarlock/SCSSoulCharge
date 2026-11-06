#pragma once
// Only DualCast staff uses game dual-cast cost. No ESP changes or global caster state.
// Fail closed if engine GMSTs are absent/invalid rather than issue free casts.
#ifndef SCS_UNIT_TEST
#include <RE/Skyrim.h>
#include <RE/G/GameSettingCollection.h>
#include <RE/S/Setting.h>
#endif
#include <cmath>
#include <limits>

namespace SpellCastingStaff {
    // Concentration and summoned weapons always use ordinary one-hand casting
    // cost and metadata, even when selected on a DualCast staff. This is shared
    // by the input preflight, projectile/effect ownership and native fire.
    [[nodiscard]] inline bool IsBoundWeaponSpell(const RE::SpellItem* spell) {
        if (!spell) return false;
        for (const auto* effect : spell->effects) {
            if (effect && effect->baseEffect &&
                effect->baseEffect->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kBoundWeapon)
                return true;
        }
        return false;
    }

    [[nodiscard]] inline bool IsDualCastEligible(const RE::SpellItem* spell) {
        return spell &&
            spell->GetCastingType() == RE::MagicSystem::CastingType::kFireAndForget &&
            !IsBoundWeaponSpell(spell);
    }

    [[nodiscard]] inline float CalculateStaffMagickaCost(RE::SpellItem* spell, RE::PlayerCharacter* actor, bool dualStaff) {
        if (!spell || !actor) return std::numeric_limits<float>::quiet_NaN();
        const float normalCost = spell->CalculateMagickaCost(actor);
        if (!dualStaff || !std::isfinite(normalCost) || normalCost < 0.0f) return normalCost;
#if defined(SCS_UNIT_TEST)
        return normalCost * 2.8f; // mock only; production reads game settings
#else
        auto* settings = RE::GameSettingCollection::GetSingleton();
        auto* base = settings ? settings->GetSetting("fMagicDualCastingCostBase") : nullptr;
        auto* multiplier = settings ? settings->GetSetting("fMagicDualCastingCostMult") : nullptr;
        if (!base || !multiplier) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        const float baseValue = base->GetFloat();
        const float multValue = multiplier->GetFloat();
        const float result = baseValue + multValue * normalCost;
        return std::isfinite(baseValue) && std::isfinite(multValue) &&
            multValue >= 0.0f && std::isfinite(result) && result >= 0.0f
            ? result : std::numeric_limits<float>::quiet_NaN();
#endif
    }
}
