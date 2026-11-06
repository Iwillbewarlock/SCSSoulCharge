#pragma once
namespace SpellCastingStaff::BoundLifecycleHook {
    // Install SE 1.5.97 call-site-only left Bound Weapon restore-slot correction.
    // Requires loaded plugin/Skyrim.esm forms (call from kDataLoaded).
    [[nodiscard]] bool Install();
}
