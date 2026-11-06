#pragma once
#include <RE/Skyrim.h>

namespace SpellCastingStaff::CasterDiagnostics {
    // These are gameplay VAV08/dual-Impact hooks, NOT diagnostic probes.
    bool Install();
    bool ArmVirtualAVHook();
    void Reset();   // F6 counters only; never unhooks gameplay vtables.
    void Summary(); // F6 summary only.

    // Bound-weapon spells have to use the physical hand as their native
    // casting source, whereas our ESP perks are gated behind virtual AV08.
    // During the synchronous native cast, temporarily recognize ONLY effects
    // from this exact staff-initiated spell/actor/hand. No actor value writes,
    // spell edits, or persistent source overrides are involved.
    class ScopedStaffBoundCast {
    public:
        ScopedStaffBoundCast(RE::PlayerCharacter* a_player, RE::SpellItem* a_spell,
                             RE::MagicSystem::CastingSource a_source);
        ~ScopedStaffBoundCast();
        ScopedStaffBoundCast(const ScopedStaffBoundCast&) = delete;
        ScopedStaffBoundCast& operator=(const ScopedStaffBoundCast&) = delete;

        [[nodiscard]] bool Matches(const RE::ActiveEffect* a_effect, const RE::Actor* a_actor) const;
        [[nodiscard]] bool InversePerkSuppressed() const { return inversePerkSuppressed_; }

    private:
        RE::PlayerCharacter* player_{};
        RE::SpellItem* spell_{};
        RE::MagicSystem::CastingSource source_{RE::MagicSystem::CastingSource::kOther};
        ScopedStaffBoundCast* previous_{};
        bool armed_{};
        bool inversePerkSuppressed_{};
    };
}
