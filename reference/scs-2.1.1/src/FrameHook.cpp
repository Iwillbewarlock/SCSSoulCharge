#include "SpellCastingStaff/RuntimeCompatibility.h"
#include "SpellCastingStaff/Controller.h"
#include "SpellCastingStaff/FrameHook.h"
#include "SpellCastingStaff/CompatibilityError.h"
#include <spdlog/spdlog.h>
#include <cstring>

namespace SpellCastingStaff::FrameHook {
    namespace {
        // Reference: user-supplied Spell Hotbar 2 / events/gameloop_hook.h.
        // The hook's caller is a main-thread per-frame location.
        REL::Relocation<void (*)()> original;
        bool installed = false;
        void Thunk() {
            // Preserve the engine's original function and any preceding trampoline.
            original();
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return;
            if (ui->GameIsPaused()) {
                if (ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME) ||
                    ui->IsMenuOpen(RE::FavoritesMenu::MENU_NAME)) {
                    Controller::Get().PreviewInventoryStaffNames();
                }
                return; // real equipment reconciliation and casting remain paused
            }
            // This is the same relocation Spell Hotbar 2 uses for frame delta.
            static const auto deltaAddress = REL::VariantID(523660, 410199, 0x30C3A08).address();
            const float dt = *reinterpret_cast<const float*>(deltaAddress);
            SpellCastingStaff::CompatibilityError::TryShowModal();
            SpellCastingStaff::Controller::Get().OnFrame(dt);
        }
    }
    bool Install() {
        if (installed) { return true; }
        // No repeated AddTask loop: one native callback per frame.
        // The shared trampoline is reserved once in Plugin.cpp before both
        // frame and menu hooks. Do NOT allocate it again here.
        auto& trampoline = SKSE::GetTrampoline();
        const REL::Relocation<std::uintptr_t> mainHook{REL::VariantID(35565, 36564, 0x5BAB10)};
        const auto site = mainHook.address() + RuntimeCompatibility::FrameCallOffset();
        if (!RuntimeCompatibility::ReadableMemory(site, 5) || *reinterpret_cast<const std::uint8_t*>(site) != 0xE8) {
            const bool hasOpcode = RuntimeCompatibility::ReadableMemory(site, 1);
            const auto opcode = hasOpcode ? *reinterpret_cast<const std::uint8_t*>(site) : 0u;
            std::uintptr_t jumpTarget = 0;
            if (opcode == 0xE9 && RuntimeCompatibility::ReadableMemory(site, 5)) {
                std::int32_t rel{};
                std::memcpy(&rel, reinterpret_cast<const void*>(site + 1), sizeof(rel));
                jumpTarget = site + 5 + static_cast<std::intptr_t>(rel);
            }
            SKSE::log::error("Frame hook rejected: expected E8 CALL at {:X} siteModule={} opcode={:02X} jumpTarget={:X} targetModule={}; E9/JMP is NOT safe to chain",
                site, RuntimeCompatibility::DescribeAddress(site), opcode,
                jumpTarget, RuntimeCompatibility::DescribeAddress(jumpTarget));
            return false;
        }
        std::int32_t displacement = 0;
        std::memcpy(&displacement, reinterpret_cast<const void*>(site + 1), sizeof(displacement));
        const auto target = site + 5 + static_cast<std::intptr_t>(displacement);
        if (!RuntimeCompatibility::ExecutableTarget(target) || (target >= site && target < site + 5) || target == reinterpret_cast<std::uintptr_t>(&Thunk)) {
            SKSE::log::error("Frame hook rejected: site={:X} target={:X} targetModule={} (invalid or recursive)",
                site, target, RuntimeCompatibility::DescribeAddress(target));
            return false;
        }
        original = trampoline.write_call<5>(site, Thunk);
        installed = true;
        SKSE::log::info("Native frame lifecycle hook installed (Spell Hotbar 2 compatible call site)");
        return true;
    }
}
