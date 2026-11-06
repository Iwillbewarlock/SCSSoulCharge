#include "SpellCastingStaff/PerkEntryProbe.h"
#include "SpellCastingStaff/CasterDiagnostics.h"
#include "SpellCastingStaff/Config.h"
#include <atomic>

namespace SpellCastingStaff::PerkEntryProbe {
namespace {
    std::atomic<bool> installed{false};
    std::atomic<bool> gameReady{false};
    std::atomic<bool> f6Capture{false};
    std::atomic<bool> f6Held{false}; // Input/reset can occur in different callbacks.
}
bool Install() {
    if (installed.load(std::memory_order_acquire)) return true;
    if (!CasterDiagnostics::Install()) {
        SKSE::log::error("VAV08 INSTALL FAILED: staff perk engine hook missing");
        return false;
    }
    installed.store(true, std::memory_order_release);
    SKSE::log::info("VAV08 effect hooks installed (diagnostic shortcut toggles logging only)");
    return true;
}
void ResetCounters(bool preserveCapture) {
    // A save/load resets the runtime effect gate, NOT the user's F6 setting.
    // Capturing() stays false while gameReady=false, so no load-time perk
    // sampling occurs against an unfinished actor/world. New game still
    // explicitly disables capture via ResetCounters(false).
    gameReady.store(false, std::memory_order_release);
    if (!preserveCapture) {
        f6Capture.store(false, std::memory_order_release);
    }
    f6Held.store(false, std::memory_order_relaxed);
    CasterDiagnostics::Reset();
}
void Arm() {
    if (!installed.load(std::memory_order_acquire)) return;
    if (!CasterDiagnostics::ArmVirtualAVHook()) {
        SKSE::log::error("VAV08 NOT READY: actor value getter hook unavailable");
        gameReady.store(false, std::memory_order_release);
        return;
    }
    gameReady.store(true, std::memory_order_release);
    const bool resumed = f6Capture.load(std::memory_order_acquire);
    if (resumed) {
        SKSE::log::warn("Diagnostic capture RESUMED after load (sample counters reset)");
    } else {
        SKSE::log::info("VAV08 ready: diagnostic capture OFF; gameplay perk hooks always enabled");
    }
}
bool Enabled() {
    return installed.load(std::memory_order_acquire) &&
           gameReady.load(std::memory_order_acquire);
}
bool Capturing() { return Enabled() && f6Capture.load(std::memory_order_relaxed); }
void OnButtonEvent(const RE::ButtonEvent* button) {
    // DirectInput keyboard scan codes. Default 64 (0x40) is F6; 0 disables toggling.
    const auto hotkey = GetConfig().diagnosticHotkey;
    if (hotkey == 0 || !button || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard ||
        button->GetIDCode() != hotkey) return;
    const bool pressed = button->Value() > 0.0f;
    if (!pressed) { f6Held.store(false, std::memory_order_relaxed); return; }
    if (f6Held.exchange(true, std::memory_order_relaxed)) return;
    if (!Enabled()) return;
    if (f6Capture.exchange(false, std::memory_order_relaxed)) {
        CasterDiagnostics::Summary();
        SKSE::log::warn("Diagnostic shortcut (scan code {}) light perk diagnostics OFF", hotkey);
        RE::DebugNotification("SpellCastingStaff: Diagnostic logs OFF");
    } else {
        CasterDiagnostics::Reset();
        f6Capture.store(true, std::memory_order_relaxed);
        SKSE::log::warn("Diagnostic shortcut (scan code {}) light perk diagnostics ON (max 32 effect samples)", hotkey);
        RE::DebugNotification("SpellCastingStaff: Diagnostic logs ON");
    }
}
} // namespace SpellCastingStaff::PerkEntryProbe
