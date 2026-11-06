#pragma once
namespace RE { class ButtonEvent; }
namespace SpellCastingStaff::PerkEntryProbe {
    bool Install();
    bool Enabled();   // Gameplay gate; diagnostic shortcut NEVER disables perk calculation.
    bool Capturing(); // User-enabled, bounded diagnostic logging only.
    // Reset runtime readiness/samples. Preserve the user diagnostic preference only across loading.
    void ResetCounters(bool preserveCapture = false);
    void Arm();
    void OnButtonEvent(const RE::ButtonEvent* button);
}
