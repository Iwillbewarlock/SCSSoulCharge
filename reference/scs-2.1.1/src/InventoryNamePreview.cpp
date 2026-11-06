#include "SpellCastingStaff/Controller.h"
#include "RE/B/BSTCreateFactoryManager.h"
#include "RE/B/BSTDerivedCreator.h"

namespace SpellCastingStaff {
    namespace {
        std::atomic_bool refreshQueued{false};
        void RefreshOpenEquipmentMenus() {
            auto* tasks = SKSE::GetTaskInterface();
            if (!tasks || refreshQueued.exchange(true, std::memory_order_acq_rel)) return;
            tasks->AddUITask([] {
                refreshQueued.store(false, std::memory_order_release);
                auto* ui = RE::UI::GetSingleton();
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!ui || !player) return;
                if (ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME)) {
                    auto menu = ui->GetMenu<RE::InventoryMenu>();
                    if (menu) {
                        if (auto* list = menu->GetRuntimeData().itemList) list->Update();
                    }
                }
                if (ui->IsMenuOpen(RE::FavoritesMenu::MENU_NAME)) {
                    auto* queue = RE::UIMessageQueue::GetSingleton();
                    auto* strings = RE::InterfaceStrings::GetSingleton();
                    if (!queue || !strings) {
                        SKSE::log::warn("NAME PREVIEW favorites refresh unavailable: UI queue/strings missing");
                        return;
                    }
                    // UIMessageQueue::CreateUIMessageData returned null in the
                    // user's SE log. Use the registered engine creator directly.
                    auto* factory = RE::MessageDataFactoryManager::GetSingleton();
                    auto* creator = factory ? factory->GetCreator<RE::InventoryUpdateData>(
                        strings->inventoryUpdateData) : nullptr;
                    if (!creator) {
                        SKSE::log::warn("NAME PREVIEW favorites refresh unavailable: InventoryUpdateData creator missing");
                        return;
                    }
                    auto* data = creator->Create();
                    if (!data) {
                        SKSE::log::warn("NAME PREVIEW favorites refresh unavailable: message factory returned null");
                        return;
                    }
                    // Native FavoritesMenu handles type 8 for the player's
                    // reference handle and a null form (refresh whole list).
                    data->unk10 = player->GetHandle().native_handle();
                    data->pad14 = 0;
                    data->unk18 = nullptr;
                    queue->AddMessage(RE::BSFixedString(RE::FavoritesMenu::MENU_NAME),
                        RE::UI_MESSAGE_TYPE::kInventoryUpdate, data);
                    SKSE::log::info("NAME PREVIEW favorites refresh queued (registered creator)");
                    // The UI message queue owns the factory-allocated data.
                }
            });
        }
    }

    // Presentation only. Do not consume syncQueued_, change hands_/memory,
    // cancel casts, refund resources, update buffs/ENCH or equip any object.
    void Controller::PreviewInventoryStaffNames() {
        if (!enabled_ || !gameReady_ || nativeCastInProgress_ || !frameHookReady_ ||
            !lifecycleReady_.load(std::memory_order_acquire)) return;
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->GameIsPaused() ||
            (!ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME) &&
             !ui->IsMenuOpen(RE::FavoritesMenu::MENU_NAME))) return;
        // Favorites equip state can become visible after the event was consumed,
        // or without an event reaching us during pause. Reconcile presentation
        // while it is open; do not use the one-shot event as the only trigger.
        const bool favorites = ui->IsMenuOpen(RE::FavoritesMenu::MENU_NAME);
        if (!favorites && !inventoryNamePreviewPending_.load(std::memory_order_acquire)) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        auto* left = player->GetEquippedObject(true);
        auto* right = player->GetEquippedObject(false);
        // Do not guess which of two overlapping staffs the gameplay policy
        // will keep. Leave the preview pending until the overlap is resolved.
        if (IsStaff(left) && IsStaff(right)) return;
        const bool pending = inventoryNamePreviewPending_.exchange(false, std::memory_order_acq_rel);
        if (!favorites && !pending) return;

        int equippedIndex = -1;
        const char* spellName = nullptr;
        for (auto* equipped : {left, right}) {
            const int index = StaffIndex(equipped);
            if (index < 0) continue;
            const auto spellID = rememberedSpells_.Get(equipped->GetFormID());
            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(spellID);
            if (!ValidSpell(player, spell, equipped)) continue;
            const char* name = spell->GetName();
            if (name && name[0]) { equippedIndex = index; spellName = name; }
        }
        bool changed = false;
        for (std::size_t i = 0; i < originalNames_.size(); ++i) {
            // Avoid copying/allocating the whole name registry every paused frame.
            const std::string_view desired = static_cast<int>(i) == equippedIndex ?
                std::string_view{spellName} : std::string_view{originalNames_[i]};
            if (appliedNames_[i] != desired) {
                ApplyTierName(player, i, std::string{desired}); // existing name-only helper
                changed = true;
            }
        }
        if (changed) {
            SKSE::log::info("NAME PREVIEW changed menu={} left={:08X} right={:08X}",
                favorites ? "FavoritesMenu" : "InventoryMenu",
                left ? left->GetFormID() : 0u, right ? right->GetFormID() : 0u);
            RefreshOpenEquipmentMenus();
        }
    }
}
