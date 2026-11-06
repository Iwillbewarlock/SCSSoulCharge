// SE 1.5.97 reference: user-supplied Immersive Staves Hooks.cpp.
// Steam 1.6.1170 call/argument setup verified against uploaded executable.
// Not the ActorEquipManager::EquipSpell entry point used unsuccessfully in v0.7.
#include "SpellCastingStaff/MenuEquipHook.h"
#include "SpellCastingStaff/RuntimeCompatibility.h"
#include "SpellCastingStaff/Controller.h"
#include <spdlog/spdlog.h>
#include <cstring>

namespace SpellCastingStaff::MenuEquipHook {
    namespace {
        // IMPORTANT: reference implementation takes BGSEquipSlot** here,
        // NOT the BGSEquipSlot* parameter of ActorEquipManager::EquipSpell.
        using MenuEquipFn = void(*)(RE::ActorEquipManager*, RE::Actor*,
                                     RE::SpellItem*, RE::BGSEquipSlot**);
        MenuEquipFn original = nullptr;
        bool installed = false;

        void Thunk(RE::ActorEquipManager* manager, RE::Actor* actor,
                   RE::SpellItem* spell, RE::BGSEquipSlot** slotPtr) {
            if (actor && actor->IsPlayerRef()) {
                SKSE::log::debug("MENU HOOK ENTER spell={:08X} slotPtr={} slot={} manager={}",
                                spell ? spell->GetFormID() : 0u,
                                static_cast<const void*>(slotPtr),
                                slotPtr ? static_cast<const void*>(*slotPtr) : nullptr,
                                static_cast<const void*>(manager));
            }
            if (actor && spell &&
                Controller::Get().TryInterceptMenuSpellEquip(actor, spell, slotPtr ? *slotPtr : nullptr)) {
                return;
            }
            // Preserve original UI equip behavior when not targeting our staff,
            // or on unsupported/spell-list exclusions.
            if (original) { original(manager, actor, spell, slotPtr); }
        }
    }

    bool Install() {
        if (installed) { return true; }
        // This location is a HEURISTIC from Immersive Staves, NOT a verified
        // ABI contract. Only install if the opcode is an ordinary near CALL.
        const REL::Relocation<std::uintptr_t> callParent{REL::RelocationID(37952, 38908)};
        const auto site = callParent.address() + 0xD7; // SE 1.5.97 and Steam 1.6.1170
        if (!RuntimeCompatibility::ReadableMemory(site, 5) ||
            *reinterpret_cast<const std::uint8_t*>(site) != 0xE8) {
            const bool hasOpcode = RuntimeCompatibility::ReadableMemory(site, 1);
            const auto opcode = hasOpcode ? *reinterpret_cast<const std::uint8_t*>(site) : 0u;
            std::uintptr_t jumpTarget = 0;
            if (opcode == 0xE9 && RuntimeCompatibility::ReadableMemory(site, 5)) {
                std::int32_t rel{};
                std::memcpy(&rel, reinterpret_cast<const void*>(site + 1), sizeof(rel));
                jumpTarget = site + 5 + static_cast<std::intptr_t>(rel);
            }
            SKSE::log::error("MENU hook rejected: site={:X} siteModule={} opcode={:02X} jumpTarget={:X} targetModule={} (not E8 CALL); vanilla path preserved",
                site, RuntimeCompatibility::DescribeAddress(site), opcode,
                jumpTarget, RuntimeCompatibility::DescribeAddress(jumpTarget));
            return false;
        }
        std::int32_t displacement = 0;
        std::memcpy(&displacement, reinterpret_cast<const void*>(site + 1), sizeof(displacement));
        const auto oldTarget = site + 5 + static_cast<std::intptr_t>(displacement);
        // Menu interception is REQUIRED. Unlike the frame hook it must keep
        // its known native ABI target; never chain an unknown menu detour.
        const bool unexpectedAE1170Target = RuntimeCompatibility::IsSupportedAE() &&
            oldTarget != REL::ID(38928).address();
        if (!RuntimeCompatibility::ExecutableTarget(oldTarget) || unexpectedAE1170Target ||
            oldTarget == site || oldTarget == reinterpret_cast<std::uintptr_t>(&Thunk)) {
            SKSE::log::error("MENU hook rejected: site={:X} target={:X} targetModule={} expectedAE={:X} reason={}",
                site, oldTarget, RuntimeCompatibility::DescribeAddress(oldTarget),
                RuntimeCompatibility::IsSupportedAE() ? REL::ID(38928).address() : 0u,
                unexpectedAE1170Target ? "unexpected AE native target (do not assume ABI compatibility)" : "invalid or recursive target");
            return false;
        }

        // Shared trampoline was allocated ONCE before FrameHook::Install.
        // Recalling SKSE::AllocTrampoline() here can release live branch stubs.
        auto& trampoline = SKSE::GetTrampoline();
        original = reinterpret_cast<MenuEquipFn>(trampoline.write_call<5>(site, &Thunk));
        installed = true;
        SKSE::log::info("MENU CALLSITE HOOK ACTIVE site={:X} original={:X} (37952/38908+0xD7)",
                        site, oldTarget);
        return true;
    }
}
