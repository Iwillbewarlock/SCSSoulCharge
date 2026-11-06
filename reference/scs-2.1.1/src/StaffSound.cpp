// SpellCastingStaff v0.6.12 - original spell MGEF SNDD cues (Spell Hotbar 2-inspired).
// No ESP/MGEF/ENCH edits, no reference to Spell Hotbar 2 audio handles.
// Native cast logic and kOther ownership logic are unchanged from v0.6.11.1.
#include "SpellCastingStaff/Controller.h"
#include <spdlog/spdlog.h>

namespace SpellCastingStaff {
namespace {

struct SpellSoundCues {
    RE::BGSSoundDescriptorForm* release{};
    RE::BGSSoundDescriptorForm* castLoop{};
};

// First non-null MGEF descriptor in the original spell's effect order,
// following Spell Hotbar 2's selection. Do not clone or mutate shared forms.
SpellSoundCues GetSpellSounds(const RE::SpellItem* spell) {
    SpellSoundCues cues{};
    if (!spell) { return cues; }
    for (const auto* effect : spell->effects) {
        if (!effect || !effect->baseEffect) { continue; }
        for (const auto& pair : effect->baseEffect->effectSounds) {
            if (!pair.sound) { continue; }
            if (pair.id == RE::MagicSystem::SoundID::kRelease && !cues.release) {
                cues.release = pair.sound;
            } else if (pair.id == RE::MagicSystem::SoundID::kCastLoop && !cues.castLoop) {
                cues.castLoop = pair.sound;
            }
        }
        if (cues.release && cues.castLoop) { break; }
    }
    return cues;
}

RE::BGSSoundDescriptorForm* ChooseFallback(const RE::SpellItem* spell,
                                            const std::array<RE::BGSSoundDescriptorForm*, 3>& sounds,
                                            const char*& category) {
    bool frost = false;
    bool shock = false;
    for (const auto* effect : spell->effects) {
        if (!effect || !effect->baseEffect) { continue; }
        if (effect->baseEffect->HasKeywordID(kShockKeywordID)) { shock = true; }
        if (effect->baseEffect->HasKeywordID(kFrostKeywordID)) { frost = true; }
    }
    category = shock ? "SHOCK" : (frost ? "FROST" : "DEFAULT_FIRE");
    return sounds[shock ? 2u : (frost ? 1u : 0u)];
}

void ResetHandle(RE::BSSoundHandle& handle) {
    handle.soundID = RE::BSSoundHandle::kInvalidID;
    handle.assumeSuccess = false;
    handle.state = RE::BSSoundHandle::AssumedState::kInitialized;
}

// Direct non-positional fallback is permitted for ONE-SHOT descriptors only.
// Loop sounds MUST retain an owned stoppable BSSoundHandle, so no direct fallback.
bool PlayDescriptor(RE::PlayerCharacter* player, RE::BGSSoundDescriptorForm* descriptor,
                    RE::BSSoundHandle* handleOut, bool isLoop) {
    if (!player || !descriptor) { return false; }
    auto* audio = RE::BSAudioManager::GetSingleton();
    if (!audio) { return false; }
    RE::BSSoundHandle handle;
    ResetHandle(handle);
    constexpr std::uint32_t kBuildFlags = 16u; // same as validated v0.6.6
    if (audio->BuildSoundDataFromDescriptor(handle, descriptor, kBuildFlags)) {
        if (auto* node = player->Get3D(); node && handle.SetPosition(player->data.location)) {
            handle.SetObjectToFollow(node);
        }
        if (handle.Play()) {
            if (handleOut) { *handleOut = handle; }
            return true;
        }
        // A handle can be allocated even if playback fails: release it.
        if (handle.IsValid()) { handle.Stop(); }
    }
    if (isLoop) { return false; }
    // One-shot fallback for Skyrim's 2D descriptor configurations.
    return audio->Play(descriptor);
}

}  // namespace

void Controller::PlayFireSound(RE::PlayerCharacter* player, RE::SpellItem* spell, bool left) {
    if (!player || !spell) { return; }
    const char* mode = "MGEF_RELEASE";
    auto* sound = GetSpellSounds(spell).release;
    if (!sound) {
        sound = ChooseFallback(spell, fireSounds_, mode);
    }
    if (!sound) {
        SKSE::log::info("SOUND {} spell={:08X} no Release SNDR or fallback",
                        left ? "L" : "R", spell->GetFormID());
        return;
    }
    // Every one-shot release is independent: never stop earlier shots.
    RE::BSSoundHandle playedHandle;
    ResetHandle(playedHandle);
    bool played = PlayDescriptor(player, sound, &playedHandle, false);
    if (!played && std::string_view(mode) == "MGEF_RELEASE") {
        sound = ChooseFallback(spell, fireSounds_, mode);
        if (sound) { played = PlayDescriptor(player, sound, &playedHandle, false); }
    }
    SKSE::log::info("SOUND {} {} spell={:08X} descriptor={:08X} played={}",
                    left ? "L" : "R", mode, spell->GetFormID(), sound ? sound->GetFormID() : 0u, played);
}

void Controller::PlayConcentrationStartSound(RE::PlayerCharacter* player, RE::SpellItem* spell, bool left) {
    if (!player || !spell) { return; }
    const auto idx = left ? 0u : 1u;
    const auto cues = GetSpellSounds(spell);
    if (cues.castLoop) {
        auto& handle = concentrationLoopHandles_[idx];
        // Stop only this plugin's old handle, never a Hotbar handle.
        if (handle.IsValid()) { handle.Stop(); }
        ResetHandle(handle);
        const bool started = PlayDescriptor(player, cues.castLoop, &handle, true);
        SKSE::log::info("SOUND CONC {} CAST_LOOP spell={:08X} descriptor={:08X} played={}",
                        left ? "L" : "R", spell->GetFormID(), cues.castLoop->GetFormID(), started);
        if (started) { return; }
        // When loop handle cannot be retained, never play the loop directly.
    }
    // If no loop is available, give ONE initial release cue (or category fallback).
    // Do not repeatedly play any SNDR during a concentration cast.
    PlayFireSound(player, spell, left);
}

void Controller::StopConcentrationSound(bool left) {
    const auto idx = left ? 0u : 1u;
    auto& handle = concentrationLoopHandles_[idx];
    if (handle.IsValid()) {
        const bool stopped = handle.Stop();
        SKSE::log::info("SOUND CONC {} CAST_LOOP_STOP stopped={}", left ? "L" : "R", stopped);
    }
    ResetHandle(handle);
}

} // namespace SpellCastingStaff
