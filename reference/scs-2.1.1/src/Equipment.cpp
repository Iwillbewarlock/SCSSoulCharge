// v0.4.7 split from v0.4.6 plugin.cpp, without intentional logic changes.
#include "SpellCastingStaff/Controller.h"
#include "SpellCastingStaff/Config.h"
#include <spdlog/spdlog.h>

namespace SpellCastingStaff {

    // A successful interception consumes the menu's spell-equip call BEFORE
    // Skyrim replaces the equipped staff. No native equip or re-equip is needed.
    bool Controller::TryInterceptMenuSpellEquip(RE::Actor* actor, RE::SpellItem* spell,
                                                const RE::BGSEquipSlot* targetSlot) {
        if (!enabled_ || !gameReady_ || !actor || !spell) {
            return false;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || actor != player) { return false; }

        // Ordinary left/right menu selection must preserve that explicit hand.
        // For a learned TWO-HANDED ritual/master spell only, the magic menu may
        // supply BothHands/no individual slot. One custom staff is unambiguous:
        // bind the ritual to THAT staff, rather than letting vanilla unequip it.
        bool left = targetSlot == slotLeft_;
        if (!targetSlot || (!left && targetSlot != slotRight_)) {
            const bool lStaff = IsStaff(player->GetEquippedObject(true));
            const bool rStaff = IsStaff(player->GetEquippedObject(false));
            if (!spell->IsTwoHanded() || lStaff == rStaff) {
                SKSE::log::info("MENU PASS spell={:08X}: ambiguous or unsupported equip slot",
                                spell->GetFormID());
                return false;
            }
            left = lStaff;
        }
        const auto* nativeEquipSlot = spell->GetEquipSlot();
        const bool restricted = spell->IsTwoHanded() ||
            (nativeEquipSlot && (nativeEquipSlot == slotLeft_ || nativeEquipSlot == slotRight_));
        auto* equipped = player->GetEquippedObject(left);
        const int tier = StaffIndex(equipped);
        if (tier < 0 || IsStaff(player->GetEquippedObject(!left)) ||
            !ValidSpell(player, spell, equipped)) {
            // Ineligible spells, unlearned spells, Toxic Toss, or missing staff:
            // let Skyrim handle the menu action normally.
            return false;
        }

        const std::size_t idx = left ? 0u : 1u;
        auto& hand = hands_[idx];
        // Paused menu preview may already show the remembered spell before
        // gameplay Sync has restored hands_. Toggling must match that selection.
        const auto assignedSpell = hand.staff == equipped->GetFormID() && hand.spell ?
            hand.spell : rememberedSpells_.Get(equipped->GetFormID());
        const bool unbind = assignedSpell == spell->GetFormID();
        CancelCharge(left, "menu spell selection");
        StopChannel(left, "menu spell selection");
        StopRapid(left, "menu spell selection");
        hand.staff = equipped->GetFormID();
        hand.spell = unbind ? 0 : spell->GetFormID();
        if (unbind) rememberedSpells_.Erase(hand.staff);
        else rememberedSpells_.Set(hand.staff, hand.spell);
        ApplyTierName(player, static_cast<std::size_t>(tier),
            unbind ? originalNames_[static_cast<std::size_t>(tier)] : SpellName(spell));
        nativeEnchantmentSyncPending_ = true;
        // Update native ENCH metadata after the binding changes. The method
        // defers if a hand caster is active and never re-equips the staff.
        SyncNativeEnchantmentProperties(player);
        QueueSync();  // update buffs/names only; never change equipped objects
        SKSE::log::info("MENU {} {} staff={:08X} spell={:08X} restricted={} '{}' (equip consumed)",
                        unbind ? "UNBIND" : "BIND",
                        left ? "L" : "R", hand.staff, hand.spell, restricted, SpellName(spell));
        const auto& config = GetConfig();
        if (unbind ? config.showSpellClearedMessage : config.showSpellSelectedMessage) {
            const char* spellName = spell->GetName();
            if (spellName && spellName[0]) {
                const std::string notice = FormatSpellSelectedMessage(
                    unbind ? config.spellClearedMessage : config.spellSelectedMessage, spellName,
                    originalNames_[static_cast<std::size_t>(tier)]);
                if (!notice.empty()) RE::DebugMessageBox(notice.c_str());
            }
        }
        return true;
    }

    void Controller::Sync() {
        if (!enabled_ || !gameReady_) { return; }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) { return; }

        auto* leftForm = player->GetEquippedObject(true);
        auto* rightForm = player->GetEquippedObject(false);
        const bool leftStaff = IsStaff(leftForm);
        const bool rightStaff = IsStaff(rightForm);

        // Distinct from the REMOVED spell-re-equip fallback: retain the original
        // one-custom-staff rule. Only when two custom staffs coexist do we
        // unequip the older one; NEVER unequip/re-equip for spell selection.
        if (leftStaff && rightStaff) {
            bool keepLeft = false;
            if (hands_[0].staff && !hands_[1].staff) {
                keepLeft = false;
            } else if (hands_[1].staff && !hands_[0].staff) {
                keepLeft = true;
            } else if (leftForm->GetFormID() == preferredStaff_ &&
                       rightForm->GetFormID() != preferredStaff_) {
                keepLeft = true;
            } else if (rightForm->GetFormID() == preferredStaff_ &&
                       leftForm->GetFormID() != preferredStaff_) {
                keepLeft = false;
            }
            const bool removeLeft = !keepLeft;
            auto* unwanted = static_cast<RE::TESObjectWEAP*>(removeLeft ? leftForm : rightForm);
            CancelCharge(removeLeft, "second custom staff equipped");
            StopChannel(removeLeft, "second custom staff equipped");
            StopRapid(removeLeft, "second custom staff equipped");
            auto* manager = RE::ActorEquipManager::GetSingleton();
            if (!manager) { return; }
            // Single-staff safety invariant. Never re-equip during spell binding.
            const bool removed = manager->UnequipObject(
                player, unwanted, nullptr, 1,
                removeLeft ? slotLeft_ : slotRight_, false, true, true, true);
            const bool gone = player->GetEquippedObject(removeLeft) != unwanted;
            // A successful return can mean an accepted/deferred request.
            // Do not self-reschedule against unchanged equipment indefinitely.
            // A later equipment event will queue reconciliation when it lands.
            if (gone) {
                QueueSync();
            } else if (!removed) {
                SKSE::log::error("Could not unequip the second custom staff");
            } else {
                SKSE::log::info("Second staff unequip pending; awaiting equipment event");
            }
            return;
        }

        // Consume only after player/equipment overlap guards; a deferred Sync
        // must not lose the pending post-load validation.
        const bool validateLoaded = validateLoadedPending_.exchange(false, std::memory_order_acq_rel);
        for (std::size_t idx = 0; idx < hands_.size(); ++idx) {
            const bool left = idx == 0;
            auto& hand = hands_[idx];
            auto* equipped = player->GetEquippedObject(left);
            if (Tier(equipped) >= 0) {
                if (hand.staff != equipped->GetFormID()) {
                    CancelCharge(left, "staff changed");
                    StopChannel(left, "staff changed");
                    StopRapid(left, "staff changed");
                    hand.staff = equipped->GetFormID();
                    hand.spell = 0;
                }
                // One post-load check only. Ordinary equipment events must not
                // cancel an active binding because HasSpell is transiently false.
                if (validateLoaded && hand.spell && !ValidSpell(player,
                        RE::TESForm::LookupByID<RE::SpellItem>(hand.spell), equipped)) {
                    CancelCharge(left, "assigned spell invalid");
                    StopChannel(left, "assigned spell invalid");
                    StopRapid(left, "assigned spell invalid");
                    hand.spell = 0;
                }
                if (!hand.spell) {
                    const auto remembered = rememberedSpells_.Get(hand.staff);
                    if (remembered) {
                        auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(remembered);
                        if (ValidSpell(player, spell, equipped)) {
                            hand.spell = remembered;
                            nativeEnchantmentSyncPending_ = true;
                        } else if (!spell ||
                            spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell ||
                            (spell->GetCastingType() != RE::MagicSystem::CastingType::kFireAndForget &&
                             spell->GetCastingType() != RE::MagicSystem::CastingType::kConcentration)) {
                            rememberedSpells_.Erase(hand.staff);
                        }
                        // Not currently learned (or another eligibility condition):
                        // keep memory; a later equipment Sync may restore it.
                    }
                }
            } else if (hand.staff || hand.spell || channels_[idx].running) {
                // Includes normal staff unequip, hand transfer, and Toxic Toss.
                // NEVER put a remembered staff back into this slot.
                CancelCharge(left, "staff unequipped");
                StopChannel(left, "staff unequipped");
                StopRapid(left, "staff unequipped");
                hand = {};
            }
        }
        UpdateBuffs(player);
        RefreshNames(player);
        SyncNativeEnchantmentProperties(player);
    }


    // Only two metadata fields are changed. NEVER copy actual MGEFs/effects,
    // charge resources, or the remembered SpellItem. There is exactly one
    // player-equipped custom staff, but all WEAP forms SHARE this ENCH;
    // its metadata is therefore global for this session until restored.
    void Controller::RestoreNativeEnchantmentProperties() {
        if (!enchantment_ || !originalNativeEnchantmentCaptured_) { return; }
        enchantment_->data.castingType = originalNativeEnchantmentCastType_;
        enchantment_->data.chargeTime = originalNativeEnchantmentChargeTime_;
        nativeEnchantmentSyncPending_ = false;
    }

    void Controller::SyncNativeEnchantmentProperties(RE::PlayerCharacter* player) {
        if (!enchantment_ || !originalNativeEnchantmentCaptured_ || !player) { return; }
        auto desiredType = originalNativeEnchantmentCastType_;
        float desiredCharge = originalNativeEnchantmentChargeTime_;
        bool targetIsBound = false;
        bool left = false;
        const bool hasLeft = IsStaff(player->GetEquippedObject(true));
        const bool hasRight = IsStaff(player->GetEquippedObject(false));
        // If neither (or both) is equipped, revert the shared ENCH.
        if (hasLeft != hasRight) {
            left = hasLeft;
            const auto idx = left ? 0u : 1u;
            auto* equipped = player->GetEquippedObject(left);
            const auto& hand = hands_[idx];
            if (equipped && hand.staff == equipped->GetFormID() && hand.spell) {
                auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(hand.spell);
                if (ValidSpell(player, spell, equipped)) {
                    const auto type = spell->GetCastingType();
                    const float charge = spell->GetChargeTime();
                    // Do not write malformed charge times to native magic data.
                    // 30 seconds is solely a guardrail, not a conversion.
                    if (std::isfinite(charge) && charge >= 0.0f && charge <= 30.0f &&
                        (type == RE::MagicSystem::CastingType::kConcentration ||
                         type == RE::MagicSystem::CastingType::kFireAndForget)) {
                        const auto* profile = Profile(equipped);
                        desiredType = profile->kind == StaffKind::Swift
                            ? RE::MagicSystem::CastingType::kConcentration : type;
                        desiredCharge = StaffChargeTime(*profile, charge);
                        targetIsBound = true;
                    } else {
                        SKSE::log::warn("ENCH SYNC skipped invalid casting metadata spell={:08X} charge={}",
                                        spell->GetFormID(), charge);
                    }
                }
            }
        }
        if (enchantment_->GetCastingType() == desiredType &&
            std::abs(enchantment_->data.chargeTime - desiredCharge) < 0.0001f) {
            nativeEnchantmentSyncPending_ = false;
            return;
        }
        if (targetIsBound) {
            // Never change a selected/charging/casting hand's native ENCH
            // while its native caster is running. Postpone until idle;
            // OnFrame checks a pending update without tasks or re-equips.
            auto* nativeCaster = player->GetMagicCaster(left
                ? RE::MagicSystem::CastingSource::kLeftHand
                : RE::MagicSystem::CastingSource::kRightHand);
            if (!nativeCaster || nativeCaster->state.underlying() != 0) {
                nativeEnchantmentSyncPending_ = true;
                return;
            }
        }
        enchantment_->data.castingType = desiredType;
        enchantment_->data.chargeTime = desiredCharge;
        nativeEnchantmentSyncPending_ = false;
        SKSE::log::info("ENCH SYNC type={} charge={:.3f}s{} (shared record, no re-equip)",
                        static_cast<int>(desiredType), desiredCharge,
                        targetIsBound ? " bound" : " restored");
    }

    bool Controller::IsStaff(const RE::TESForm* form) const {
        if (!form || !keyword_) return false;
        const auto* weapon = form->As<RE::TESObjectWEAP>();
        return weapon && weapon->HasKeyword(keyword_);
    }

    const StaffProfile* Controller::Profile(const RE::TESForm* form) const {
        if (!IsStaff(form)) return nullptr;
        const auto* weapon = form->As<RE::TESObjectWEAP>();
        const StaffProfile* found = nullptr;
        for (std::size_t i = 0; i < profileKeywords_.size(); ++i) {
            if (profileKeywords_[i] && weapon->HasKeyword(profileKeywords_[i])) {
                if (found) return nullptr; // Reject conflicting type/tier keywords.
                found = &kStaffProfiles[i];
            }
        }
        return found;
    }

    int Controller::Tier(const RE::TESForm* form) const {
        const auto* p = Profile(form);
        return p ? p->tier : -1;
    }

    int Controller::StaffIndex(const RE::TESForm* form) const {
        if (!form) return -1;
        for (std::size_t i = 0; i < staffs_.size(); ++i) {
            if (staffs_[i] == form) return static_cast<int>(i);
        }
        return -1;
    }

    bool Controller::ValidSpell(RE::PlayerCharacter* player, RE::SpellItem* spell,
                                const RE::TESForm* staff) const {
        const auto* p = Profile(staff);
        if (!p || !player || !spell || !player->HasSpell(spell) ||
            (toxicThrowSpell_ && spell == toxicThrowSpell_)) return false;
        if (spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell ||
            (spell->GetCastingType() != RE::MagicSystem::CastingType::kFireAndForget &&
             spell->GetCastingType() != RE::MagicSystem::CastingType::kConcentration)) return false;
        // All staff kinds may bind learned ritual/master spells. The actual
        // native spell cast is still subject to Skyrim's engine validation.
        return true;
    }

    void Controller::UpdateBuffs(RE::PlayerCharacter* player) {
        if (!player) return;
        const StaffProfile* selected = nullptr;
        // In compatibility-error mode never (re)grant any staff-buff ability.
        // Only one custom staff can be equipped; preserve the original
        // defensive highest-tier tie breaker without mixing buff banks.
        for (const bool left : {true, false}) {
            if (HasCompatibilityFailure()) break;
            if (const auto* profile = Profile(player->GetEquippedObject(left));
                profile && (!selected || profile->tier > selected->tier)) {
                selected = profile;
            }
        }
        const int strongest = selected ? selected->tier : -1;
        RE::SpellItem* wanted = nullptr;
        if (selected) {
            const auto index = static_cast<std::size_t>(strongest);
            switch (selected->kind) {
                case StaffKind::Basic:
                    if (index < buffs_.size()) wanted = buffs_[index];
                    break;
                case StaffKind::Stamina:
                    if (strongest >= 1 && strongest <= 5) wanted = staminaBuffs_[index - 1];
                    break;
                case StaffKind::Swift:
                    if (strongest >= 2 && strongest <= 5) wanted = swiftBuffs_[index - 2];
                    break;
                case StaffKind::Focus:
                    if (strongest >= 2 && strongest <= 5) wanted = focusBuffs_[index - 2];
                    break;
                case StaffKind::DualCast:
                    if (strongest >= 3 && strongest <= 5) wanted = dualCastBuffs_[index - 3];
                    break;
                case StaffKind::Eternity:
                    if (strongest >= 1 && strongest <= 5) wanted = eternityBuffs_[index - 1];
                    break;
            }
        }
        // Repair against all known banks, not merely the selected one: a
        // previous staff's active ability must never survive a type switch.
        bool repair = buffTier_ != strongest || appliedBuff_ != wanted;
        const auto checkBank = [&](const auto& bank) {
            for (auto* spell : bank) {
                if (spell && player->HasSpell(spell) != (spell == wanted)) repair = true;
            }
        };
        checkBank(buffs_);
        checkBank(staminaBuffs_);
        checkBank(swiftBuffs_);
        checkBank(focusBuffs_);
        checkBank(dualCastBuffs_);
        checkBank(eternityBuffs_);
        if (!repair) return;
        const auto removeBank = [&](const auto& bank) {
            for (auto* spell : bank) {
                if (spell && player->HasSpell(spell)) player->RemoveSpell(spell);
            }
        };
        removeBank(buffs_);
        removeBank(staminaBuffs_);
        removeBank(swiftBuffs_);
        removeBank(focusBuffs_);
        removeBank(dualCastBuffs_);
        removeBank(eternityBuffs_);
        if (wanted) player->AddSpell(wanted);
        buffTier_ = strongest;
        appliedBuff_ = wanted;
        SKSE::log::info("Buff kind={} tier={} spell={:08X}",
            selected ? static_cast<int>(selected->kind) : -1,
            strongest, wanted ? wanted->GetFormID() : 0u);
    }

} // namespace SpellCastingStaff
