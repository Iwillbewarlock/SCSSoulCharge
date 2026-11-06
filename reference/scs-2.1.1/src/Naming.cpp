// v0.4.7 split from v0.4.6 plugin.cpp, without intentional logic changes.
#include "SpellCastingStaff/Controller.h"
#include <spdlog/spdlog.h>

namespace SpellCastingStaff {

    void Controller::ApplyTierName(RE::PlayerCharacter* player, std::size_t tier, const std::string& name) {
        if (tier >= staffs_.size() || !staffs_[tier] || name.empty()) {
            return;
        }
        if (appliedNames_[tier] != name) {
            static_cast<RE::TESFullName*>(staffs_[tier])->fullName = name.c_str();
            appliedNames_[tier] = name;
            SKSE::log::info("WEAP FULL tier {} -> {}", tier, name);
        }
        // Legacy v0.4.x renamed a stack's ExtraTextDisplayData rather
        // than WEAP FULL. Keep any surviving instance override in sync
        // so SkyUI and Party Sheet won't disagree. Iterate only a form's
        // existing extras; never synthesize ExtraDataList or worn markers.
        if (player) {
            if (auto* changes = player->GetInventoryChanges(); changes && changes->entryList) {
                for (auto* entry : *changes->entryList) {
                    if (!entry || entry->object != staffs_[tier] || !entry->extraLists) {
                        continue;
                    }
                    for (auto* extra : *entry->extraLists) {
                        if (extra) {
                            if (auto* legacy = extra->GetByType<RE::ExtraTextDisplayData>()) {
                                // Updating even when the base name is unchanged
                                // cleans stale custom names from old DLL saves.
                                const char* oldCustom = legacy->displayName.c_str();
                                if (!oldCustom || std::string_view(oldCustom) != std::string_view(name)) {
                                    legacy->SetName(name.c_str());
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    void Controller::RestoreAllNames() {
        for (std::size_t i = 0; i < staffs_.size(); ++i) {
            if (staffs_[i] && !originalNames_[i].empty() &&
                appliedNames_[i] != originalNames_[i]) {
                static_cast<RE::TESFullName*>(staffs_[i])->fullName = originalNames_[i].c_str();
                appliedNames_[i] = originalNames_[i];
            }
        }
    }

    void Controller::RefreshNames(RE::PlayerCharacter* player) {
        if (!player) {
            return;
        }
        auto desired = originalNames_;
        for (std::size_t h = 0; h < hands_.size(); ++h) {
            auto& hand = hands_[h];
            const bool left = h == 0;
            auto* equipped = player->GetEquippedObject(left);
            int tier = StaffIndex(equipped);
            if (tier < 0 || !hand.spell) {
                continue;
            }
            if (auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(hand.spell)) {
                if (const char* name = spell->GetName(); name && name[0]) {
                    desired[static_cast<std::size_t>(tier)] = name;
                }
            }
        }
        for (std::size_t i = 0; i < desired.size(); ++i) {
            ApplyTierName(player, i, desired[i]);
        }
    }

} // namespace SpellCastingStaff
