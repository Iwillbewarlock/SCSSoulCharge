// v0.4.7 split from v0.4.6 plugin.cpp, without intentional logic changes.
#include "SpellCastingStaff/Controller.h"
#include "SpellCastingStaff/ProjectileDualFlag.h"
#include "SpellCastingStaff/CasterDiagnostics.h"
#include "SpellCastingStaff/PerkEntryProbe.h"
#include <spdlog/spdlog.h>
#include "SpellCastingStaff/Config.h"
#include "SpellCastingStaff/DualCastCost.h"
#include "SpellCastingStaff/StaffPerkPolicy.h"
#include <RE/B/BSAnimationGraphManager.h>

namespace SpellCastingStaff {
    Controller& Controller::Get() {
        static Controller controller;
        return controller;
    }

    // The native kOther origin remains the v1.2.15 VAV08 rule. Restrict
    // ONLY the Swift/DualCast compensation PERKs to eligible F&F casts.
    // Check binding identity to avoid suppressing unrelated spell effects.
    bool Controller::SuppressInversePerkForSpell(const RE::SpellItem* spell) const {
        if (!enabled_ || !gameReady_ || !spell) return false;
        const bool ff = spell->GetCastingType() == RE::MagicSystem::CastingType::kFireAndForget;
        const bool bound = IsBoundWeaponSpell(spell);
        if (ff && !bound) return false;
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) return false;
        for (std::size_t i = 0; i < hands_.size(); ++i) {
            const auto* equipped = pc->GetEquippedObject(i == 0);
            if (!equipped || equipped->GetFormID() != hands_[i].staff ||
                hands_[i].spell != spell->GetFormID()) continue;
            const auto* profile = Profile(equipped);
            if (profile && !ShouldApplyStaffPerk(profile->kind, ff, bound)) return true;
        }
        return false;
    }

    bool Controller::OwnsDualCastEffect(const RE::ActiveEffect* effect) const {
        if (!enabled_ || !gameReady_ || !effect ||
            effect->castingSource != RE::MagicSystem::CastingSource::kOther ||
            !effect->spell ||
            effect->spell->GetCastingType() != RE::MagicSystem::CastingType::kFireAndForget) return false;
        const auto caster = effect->GetCasterActor();
        return OwnsDualCastSpell(caster.get(), effect->spell, effect->castingSource);
    }

    bool Controller::OwnsDualCastSpell(RE::TESObjectREFR* caster, RE::MagicItem* spell,
                                      RE::MagicSystem::CastingSource source) const {
        if (!enabled_ || !gameReady_ || !spell ||
            spell->GetCastingType() != RE::MagicSystem::CastingType::kFireAndForget ||
            source != RE::MagicSystem::CastingSource::kOther || SpellHotbarClaimsOther()) return false;
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc || caster != pc) return false;
        for (unsigned i = 0; i < 2; ++i) {
            auto* equipped = pc->GetEquippedObject(i == 0);
            if (!equipped || !hands_[i].staff || !hands_[i].spell ||
                equipped->GetFormID() != hands_[i].staff ||
                spell->GetFormID() != hands_[i].spell) continue;
            const auto* profile = Profile(equipped);
            // A matching bound weapon is NOT a dual cast (ordinary cost and flags).
            const auto* selected = RE::TESForm::LookupByID<RE::SpellItem>(hands_[i].spell);
            if (profile && profile->kind == StaffKind::DualCast &&
                selected && IsDualCastEligible(selected)) return true;
        }
        return false;
    }

    bool Controller::ResolveForms() {
        if (enabled_) { return true; }
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return false;
        }
        keyword_ = dh->LookupForm<RE::BGSKeyword>(kKeywordID, kPluginName);
        for (std::size_t i = 0; i < kStaffProfiles.size(); ++i) {
            profileKeywords_[i] = dh->LookupForm<RE::BGSKeyword>(kStaffProfiles[i].keywordID, kPluginName);
            if (!profileKeywords_[i]) {
                SKSE::log::critical("Missing staff keyword {:08X}; install the matching 27-staff ESP", kStaffProfiles[i].keywordID);
                return false;
            }
        }
        for (std::size_t i = 0; i < kBuffIDs.size(); ++i) {
            buffs_[i] = dh->LookupForm<RE::SpellItem>(kBuffIDs[i], kPluginName);
            if (!buffs_[i]) {
                SKSE::log::critical("Missing staff buff tier {}", i);
                return false;
            }
        }
        for (std::size_t i = 0; i < kStaminaBuffIDs.size(); ++i) {
            staminaBuffs_[i] = dh->LookupForm<RE::SpellItem>(kStaminaBuffIDs[i], kPluginName);
            if (!staminaBuffs_[i]) {
                SKSE::log::critical("Missing Stamina staff buff tier {} (local {:06X})", i + 1, kStaminaBuffIDs[i]);
                return false;
            }
        }
        const auto resolveBuffBank = [&](auto& bank, const auto& ids, std::string_view label) -> bool {
            for (std::size_t i = 0; i < ids.size(); ++i) {
                bank[i] = dh->LookupForm<RE::SpellItem>(ids[i], kPluginName);
                if (!bank[i]) {
                    SKSE::log::critical("Missing {} staff buff at local {:06X}", label, ids[i]);
                    return false;
                }
            }
            return true;
        };
        if (!resolveBuffBank(swiftBuffs_, kSwiftBuffIDs, "Swift") ||
            !resolveBuffBank(focusBuffs_, kFocusBuffIDs, "Focus") ||
            !resolveBuffBank(dualCastBuffs_, kDualCastBuffIDs, "DualCast") ||
            !resolveBuffBank(eternityBuffs_, kEternityBuffIDs, "Eternity")) return false;
        staffs_.clear(); originalNames_.clear(); appliedNames_.clear();
        // Common keyword recognition supports additional WEAP records using an existing profile.
        for (auto* staff : dh->GetFormArray<RE::TESObjectWEAP>()) {
            if (!IsStaff(staff)) continue;
            staffs_.push_back(staff);
            const char* name = staff->GetName();
            originalNames_.emplace_back(name ? name : "");
            appliedNames_.push_back(originalNames_.back());
            if (!Profile(staff)) {
                SKSE::log::warn("Staff {:08X} has no unambiguous tier keyword; casting disabled", staff->GetFormID());
            }
        }
        enchantment_ = dh->LookupForm<RE::EnchantmentItem>(kEnchantmentID, kPluginName);
        for (auto& handle : concentrationLoopHandles_) {
            handle.soundID = RE::BSSoundHandle::kInvalidID;
            handle.assumeSuccess = false;
            handle.state = RE::BSSoundHandle::AssumedState::kInitialized;
        }
        for (std::size_t i = 0; i < kFireSoundIDs.size(); ++i) {
            fireSounds_[i] = dh->LookupForm<RE::BGSSoundDescriptorForm>(kFireSoundIDs[i], "Skyrim.esm");
            if (!fireSounds_[i]) {
                SKSE::log::warn("Missing Skyrim.esm staff fire SNDR {:08X}; sound category {} disabled",
                                kFireSoundIDs[i], i);
            }
        }
        slotRight_ = dh->LookupForm<RE::BGSEquipSlot>(0x13F42, "Skyrim.esm");
        slotLeft_ = dh->LookupForm<RE::BGSEquipSlot>(0x13F43, "Skyrim.esm");
        if (!keyword_ || !enchantment_ || !slotRight_ || !slotLeft_) {
            SKSE::log::critical("Missing SpellCastingStaff keyword/enchantment or equip slots");
            return false;
        }
        // This ENCH is shared among all custom staff WEAP records. Capture its
        // original runtime metadata ONCE for safe restore on unequip/load.
        if (!originalNativeEnchantmentCaptured_) {
            originalNativeEnchantmentCastType_ = enchantment_->GetCastingType();
            originalNativeEnchantmentChargeTime_ = enchantment_->GetChargeTime();
            originalNativeEnchantmentCaptured_ = true;
        }
        for (auto* staff : staffs_) {
            if (!staff->HasKeyword(keyword_)) {
                SKSE::log::critical("SpellCastingStaff tier {:08X} has no SpellCastingStaff_Keyword",
                                    staff->GetFormID());
                return false;
            }
        }
        // Resolve only after all plugins have loaded. No master dependency,
        // no assumptions about Toxic Toss scripts or its presence.
        toxicThrowSpell_ = dh->LookupForm<RE::SpellItem>(kToxicTossSpellID, kToxicTossFile);
        if (toxicThrowSpell_) {
            SKSE::log::info("Optional Toxic Toss support enabled ({} / {:08X})", kToxicTossFile,
                            toxicThrowSpell_->GetFormID());
        } else {
            SKSE::log::info("Toxic Toss throw spell not resolved; integration disabled (Toxic Toss.esp absent or local spell ID 0x12C8 missing)");
        }
        // The uploaded ESP already has the matching GLOB and AND conditions:
        // BuffofStaffStamina01..05 / IsCasting == 1 / GetGlobalValue == 1.
        // Fail closed on an older mismatched ESP instead of silently claiming
        // to suppress regen while still affecting the opposite-hand spell.
        staminaCastingGlobal_ = dh->LookupForm<RE::TESGlobal>(0x09B7, kPluginName);
        if (!staminaCastingGlobal_) {
            SKSE::log::critical("Missing SCS_StaminaRateZero Global (local 0x09B7); install matching ESP");
            return false;
        }
        ResetStaminaCastingGlobal();
        // Spell Hotbar 2 uses this GLOb to select which player MagicCaster
        // is used for an active animation-driven spell. Nonzero kOther (2)
        // means do NOT start a competing SpellCastingStaff channel.
        spellHotbarCastingSource_ = dh->LookupForm<RE::TESGlobal>(0x0835, "SpellHotbar.esp");
        SKSE::log::info("Spell Hotbar 2 kOther reservation GLOb available={}",
                        spellHotbarCastingSource_ != nullptr);
        enabled_ = true;
        nativeEnchantmentSyncPending_ = true;
        SKSE::log::info("SpellCastingStaff ready: {} keyword staffs; learned spells accepted by all tiers; ENCH type={} charge={:.3f}s",
                        staffs_.size(), static_cast<int>(originalNativeEnchantmentCastType_), originalNativeEnchantmentChargeTime_);
        return true;
    }

    void Controller::Attach() {
        if (!enabled_) {
            return;
        }
        if (auto* events = RE::ScriptEventSourceHolder::GetSingleton(); events && !equipSinkAdded_) {
            events->AddEventSink<RE::TESEquipEvent>(this);
            equipSinkAdded_ = true;
        }
        RegisterPlayerAnimationSink();
        if (auto* ui = RE::UI::GetSingleton(); ui && !menuSinkAdded_) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(this);
            menuSinkAdded_ = true;
        }
        QueueSync();
    }

    void Controller::DisableForCompatibilityFailure() {
        compatibilityFailure_.store(true, std::memory_order_release);
        frameHookReady_ = false;
        gameReady_ = false;
        lifecycleReady_.store(false, std::memory_order_release);
        // Do not touch the player at kDataLoaded: the game world may not yet exist.
        // Purge staff buffs at NewGame/PostLoadGame when actor data is ready.
        RestoreNativeEnchantmentProperties();
        ResetStaminaCastingGlobal();
    }

    void Controller::CleanupDisabledStaffBuffsAfterLoad() {
        if (!HasCompatibilityFailure()) return;
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            UpdateBuffs(player); // Final check after player/world becomes available.
        }
    }

    void Controller::OnPostLoadGame() {
        validateLoadedPending_.store(true, std::memory_order_release);
        ProjectileDualFlag::Reset();
        gameReady_ = true;
        ++syncGeneration_;
        syncQueued_ = false;
        inventoryNamePreviewPending_ = false;
        RefundLoadedCharge();
        RestoreNativeEnchantmentProperties();
        StopConcentrationSound(true);
        StopConcentrationSound(false);
        channels_ = {};
        rapid_ = {};
        pendingFire_ = {};
        charges_ = {};
        gateHeld_ = {};
        gateBlocked_ = {};
        drawOnlyPress_ = {};
        nativeCastInProgress_ = false;
        boundLeftDummyHandoff_ = false;
        ResetStaminaCastingGlobal();
        for (auto& value : stagedInput_) value.store(0, std::memory_order_release);
        for (auto& value : nativeAttackSequence_) value.store(0, std::memory_order_release);
        attackHeld_ = {};
        attackStateKnown_ = {};
        firedThisPress_ = {};
        attackPressedAt_ = {};
        preferredStaff_ = 0;
        animationSinkCheckCountdown_ = 0.0f;
        animationSinkStartupRetryRemaining_ = 0.0f;
        animationSinkStartupDeferred_ = false;
        // Revert stale runtime FULL values before co-save bindings are re-applied.
        RestoreAllNames();
        RegisterPlayerAnimationSink();
        if (animationSinkStartupDeferred_) {
            animationSinkCheckCountdown_ = 1.0f;
            animationSinkStartupRetryRemaining_ = 30.0f;
            SKSE::log::info("ANIM SINK startup retry window armed (post-load)");
        }
        // Existing co-save binding is reconciled by Sync() without re-equipping.
        // Revalidate abilities even if tier did not change during loading.
        buffTier_ = -1;
        appliedBuff_ = nullptr;
        nativeEnchantmentSyncPending_ = true;
        if (HasCompatibilityFailure()) {
            DisableForCompatibilityFailure();
            if (auto* player = RE::PlayerCharacter::GetSingleton()) UpdateBuffs(player);
            return;
        }
        lifecycleReady_.store(true, std::memory_order_release);
        QueueSync();
    }

    void Controller::OnNewGame() {
        ProjectileDualFlag::Reset();
        gameReady_ = true;
        ++syncGeneration_;
        syncQueued_ = false;
        inventoryNamePreviewPending_ = false;
        loadedChargeRefund_ = 0.0f;
        loadedStaminaRefund_ = 0.0f;
        RestoreNativeEnchantmentProperties();
        StopConcentrationSound(true);
        StopConcentrationSound(false);
        channels_ = {};
        rapid_ = {};
        pendingFire_ = {};
        charges_ = {};
        gateHeld_ = {};
        gateBlocked_ = {};
        drawOnlyPress_ = {};
        nativeCastInProgress_ = false;
        boundLeftDummyHandoff_ = false;
        ResetStaminaCastingGlobal();
        for (auto& value : stagedInput_) value.store(0, std::memory_order_release);
        for (auto& value : nativeAttackSequence_) value.store(0, std::memory_order_release);
        attackHeld_ = {};
        attackStateKnown_ = {};
        firedThisPress_ = {};
        attackPressedAt_ = {};
        preferredStaff_ = 0;
        animationSinkCheckCountdown_ = 0.0f;
        animationSinkStartupRetryRemaining_ = 0.0f;
        animationSinkStartupDeferred_ = false;
        RestoreAllNames();
        hands_ = {};
        rememberedSpells_.Clear();
        validateLoadedPending_.store(false, std::memory_order_release);
        buffTier_ = -1;
        appliedBuff_ = nullptr;
        RegisterPlayerAnimationSink();
        if (animationSinkStartupDeferred_) {
            animationSinkCheckCountdown_ = 1.0f;
            animationSinkStartupRetryRemaining_ = 30.0f;
            SKSE::log::info("ANIM SINK startup retry window armed (new game)");
        }
        nativeEnchantmentSyncPending_ = true;
        if (HasCompatibilityFailure()) {
            DisableForCompatibilityFailure();
            if (auto* player = RE::PlayerCharacter::GetSingleton()) UpdateBuffs(player);
            return;
        }
        lifecycleReady_.store(true, std::memory_order_release);
        QueueSync();
    }

    void Controller::OnRevert() {
        PerkEntryProbe::ResetCounters(true); // Revert during load preserves user F6 choice.
        lifecycleReady_.store(false, std::memory_order_release);
        ProjectileDualFlag::Reset();
        gameReady_ = false;
        ++syncGeneration_;
        // Discard old-world escrow without touching the newly loaded actor.
        loadedChargeRefund_ = 0.0f;
        loadedStaminaRefund_ = 0.0f;
        RestoreNativeEnchantmentProperties();
        StopConcentrationSound(true);
        StopConcentrationSound(false);
        // During save/load the player caster can be invalid. Never call InterruptCast here.
        RestoreAllNames();
        channels_ = {};
        rapid_ = {};
        pendingFire_ = {};
        charges_ = {};
        gateHeld_ = {};
        gateBlocked_ = {};
        drawOnlyPress_ = {};
        nativeCastInProgress_ = false;
        boundLeftDummyHandoff_ = false;
        ResetStaminaCastingGlobal();
        for (auto& value : stagedInput_) value.store(0, std::memory_order_release);
        for (auto& value : nativeAttackSequence_) value.store(0, std::memory_order_release);
        attackHeld_ = {};
        attackStateKnown_ = {};
        firedThisPress_ = {};
        attackPressedAt_ = {};
        preferredStaff_ = 0;
        animationSinkCheckCountdown_ = 0.0f;
        animationSinkStartupRetryRemaining_ = 0.0f;
        animationSinkStartupDeferred_ = false;
        hands_ = {};
        rememberedSpells_.Clear();
        validateLoadedPending_.store(false, std::memory_order_release);
        buffTier_ = -1;
        appliedBuff_ = nullptr;
        syncQueued_ = false;
        inventoryNamePreviewPending_ = false;
        nativeEnchantmentSyncPending_ = false;
    }

    void Controller::AttachInput() {
        if (inputSinkAdded_) {
            return;
        }
        if (auto* source = RE::BSInputDeviceManager::GetSingleton()) {
            source->AddEventSink(this);
            inputSinkAdded_ = true;
            SKSE::log::info("Input sink attached for concentration release");
        }
    }

    void Controller::QueueSync() {
        if (!enabled_ || !lifecycleReady_.load(std::memory_order_acquire)) return;
        // No SKSE AddTask: those tasks were observed executing on different
        // native callback threads, concurrently with the input event sink.
        // A queued request has no world references and is cleared at revert.
        syncQueued_.store(true, std::memory_order_release);
    }

    void Controller::RegisterPlayerAnimationSink() {
        if (!enabled_ || !gameReady_) { return; }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) { return; }

        // kNewGame may arrive before the player's animation graph exists.
        // CommonLibSSE-NG 3.6.0 Actor::AddAnimationGraphEventSink() does not
        // register a sink without a manager, and dereferences graphs.front()
        // if a manager exists but has no graphs. Validate both first.
        RE::BSAnimationGraphManagerPtr graphManager;
        if (!player->GetAnimationGraphManager(graphManager) ||
            !graphManager || graphManager->graphs.empty() ||
            std::any_of(graphManager->graphs.begin(), graphManager->graphs.end(),
                        [](const auto& graph) { return !graph; })) {
            if (!animationSinkStartupDeferred_) {
                SKSE::log::info("ANIM SINK registration deferred: player animation graph not ready");
                animationSinkStartupDeferred_ = true;
            }
            return;
        }

        // Idempotent in CommonLibSSE-NG 3.6.0: scans the graph's existing
        // sinks, adding only if absent. A false return ALSO means 'already
        // registered', so it is not an error. Retrying handles graph rebuilds.
        if (player->AddAnimationGraphEventSink(this)) {
            SKSE::log::info("ANIM SINK attached to player graph (new game/load/rebuild)");
        }
        if (animationSinkStartupDeferred_) {
            SKSE::log::info("ANIM SINK player graph became available after startup");
            animationSinkStartupDeferred_ = false;
        }
    }

} // namespace SpellCastingStaff
