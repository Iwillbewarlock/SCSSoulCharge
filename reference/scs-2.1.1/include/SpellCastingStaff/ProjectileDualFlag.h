#pragma once
// F&F dual-cast projectile metadata (v1.2.5i verified path). Only called on the existing
// main-thread frame hook; no new vtable/inline/global condition patching.
#include <RE/Skyrim.h>

namespace SpellCastingStaff::ProjectileDualFlag {
    // Arm after a successful native F&F kOther cast belonging to a DualCast staff.
    // The exact SpellItem pointer is kept only for a bounded, in-world window.
    void Arm(RE::PlayerCharacter* player, RE::SpellItem* spell);
    void Update();
    // Called before the world is changed. Never dereferences old state.
    void Reset();
}
