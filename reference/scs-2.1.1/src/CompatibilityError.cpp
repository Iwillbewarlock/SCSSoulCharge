#include "SpellCastingStaff/CompatibilityError.h"
#include "SpellCastingStaff/Controller.h"
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <atomic>
#include <string>
#include <string_view>
#include <spdlog/spdlog.h>

namespace SpellCastingStaff::CompatibilityError {
namespace {
    std::atomic<bool> menuFailed{false};
    std::atomic<bool> showAfterLoad{false};
    std::atomic<bool> shown{false};
    // Owned storage must outlive the deferred SKSE UI task.  The name is set
    // once during SKSEPlugin_Load, before the UI is ever scheduled.
    std::string modalText;
}
void SetLogFileName(std::string_view fileName) {
    modalText =
        "SpellCastingStaff - Compatibility Error\n\n"
        "The required Magic Menu hook could not be installed.\n"
        "Spellcasting Staff is DISABLED for this game session.\n\n"
        "Check Documents\\My Games\\Skyrim Special Edition\\SKSE\\" +
        std::string(fileName) + " for 'MENU hook rejected'.\n"
        "A conflicting SKSE plugin or a different game executable may be responsible.\n\n"
        "Exit the game, resolve the conflict, and restart Skyrim.";
}
void MarkMenuHookFailed() {
    menuFailed.store(true, std::memory_order_release);
    SKSE::log::critical("REQUIRED MENU HOOK FAILED: casting permanently disabled for this process; modal queued for NewGame/PostLoadGame");
}
void OnGameReady() {
    if (menuFailed.load(std::memory_order_acquire) && !shown.load(std::memory_order_acquire))
        showAfterLoad.store(true, std::memory_order_release);
}
void TryShowModal() {
    if (!showAfterLoad.load(std::memory_order_acquire) || shown.load(std::memory_order_acquire)) return;
    auto* ui = RE::UI::GetSingleton();
    if (!ui || ui->GameIsPaused() || !RE::PlayerCharacter::GetSingleton()) return;
    if (shown.exchange(true, std::memory_order_acq_rel)) return;
    SpellCastingStaff::Controller::Get().CleanupDisabledStaffBuffsAfterLoad();
    showAfterLoad.store(false, std::memory_order_release);
    SKSE::log::critical("Showing SpellCastingStaff required-menu-hook compatibility error dialog");
    // Dispatch UI creation outside the native Main::Update hook stack.
    if (auto* tasks = SKSE::GetTaskInterface()) {
        tasks->AddTask([] { RE::DebugMessageBox(modalText.c_str()); });
    } else {
        // Do not invoke game UI synchronously from the native frame thunk.
        SKSE::log::critical("Task interface unavailable; compatibility modal cannot be queued");
    }
}
} // namespace SpellCastingStaff::CompatibilityError
