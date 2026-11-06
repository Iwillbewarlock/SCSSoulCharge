#pragma once
#include <string_view>
namespace SpellCastingStaff::CompatibilityError {
    // Called once at startup, before any game UI task can be queued.
    void SetLogFileName(std::string_view fileName);
    // Called ONLY when the required native Magic Menu hook was rejected.
    void MarkMenuHookFailed();
    void OnGameReady();
    // Called once per unpaused game frame from the known native main-thread hook.
    void TryShowModal();
}
