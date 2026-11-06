#pragma once
#include "SpellCastingStaff/StaffProfile.h"
namespace SpellCastingStaff {
// Only Swift (Rapid) and DualCast (Amplification) require F&F-only
// virtual perk gating. Others preserve their existing all-spell bonuses.
[[nodiscard]] constexpr bool ShouldApplyStaffPerk(StaffKind kind, bool fireAndForget, bool boundWeapon) {
    if (kind == StaffKind::Swift || kind == StaffKind::DualCast)
        return fireAndForget && !boundWeapon;
    return true;
}
}
