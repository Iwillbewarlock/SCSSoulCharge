#pragma once
// Pure policy shared by the Skyrim plugin and platform-independent tests.
// Caller MUST validate actor, native callsite, manager, count, staff identity,
// and original slot before supplying the positive evidence below.
namespace SpellCastingStaff::BoundRestorePolicy {
    struct Evidence {
        bool verifiedFinishingLeftBound = false; // exact ActiveEffect from Finish, NOT another effect
        bool leftHandCurrentlyBound = false;
        bool restoredItemIsOurStaff = false;
        bool slotUnspecified = false;
        bool countOne = false;
    };
    [[nodiscard]] constexpr bool ShouldCorrect(const Evidence& e) noexcept {
        return e.verifiedFinishingLeftBound && e.leftHandCurrentlyBound &&
               e.restoredItemIsOurStaff && e.slotUnspecified && e.countOne;
    }
}
