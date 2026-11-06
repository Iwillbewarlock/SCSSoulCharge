#include "SpellCastingStaff/RuntimeCompatibility.h"
#include "SpellCastingStaff/AttackHook.h"
#include "SpellCastingStaff/Controller.h"
#include <spdlog/spdlog.h>
#include <RE/A/AttackBlockHandler.h>
#include <RE/P/PlayerControls.h>

namespace SpellCastingStaff::AttackHook {
    namespace {
        // ProcessButton: SE/1170 slot 04; 1.7.104 adds two input-handler slots (06).
        // Hook the existing vtable entry and chain it, including earlier hooks.
        using Process = void (*)(RE::AttackBlockHandler*, RE::ButtonEvent*, RE::PlayerControlsData*);
        REL::Relocation<Process> original;
        bool installed = false;
        void Thunk(RE::AttackBlockHandler* handler, RE::ButtonEvent* button,
                   RE::PlayerControlsData* data) {
            if (Controller::Get().AllowStaffAttack(button)) {
                original(handler, button, data);
            }
        }
    }
    bool Install() {
        if (installed) { return true; }
        auto* controls = RE::PlayerControls::GetSingleton();
        if (!controls || !controls->attackBlockHandler) {
            SKSE::log::error("Staff attack gate unavailable: no AttackBlockHandler");
            return false;
        }
        const auto slot = RuntimeCompatibility::AttackButtonSlot();
        const auto tableAddress = *reinterpret_cast<std::uintptr_t*>(controls->attackBlockHandler);
        if (!RuntimeCompatibility::ReadableMemory(tableAddress, (slot + 1) * sizeof(std::uintptr_t))) {
            SKSE::log::error("Staff attack gate rejected: missing vtable");
            return false;
        }
        const auto target = reinterpret_cast<const std::uintptr_t*>(tableAddress)[slot];
        if (!RuntimeCompatibility::ExecutableTarget(target) || target == reinterpret_cast<std::uintptr_t>(&Thunk)) {
            SKSE::log::error("Staff attack gate rejected: slotTarget={:X} targetModule={} (invalid/recursive)",
                target, RuntimeCompatibility::DescribeAddress(target));
            return false;
        }
        REL::Relocation<std::uintptr_t> table{tableAddress};
        original = table.write_vfunc(slot, Thunk);
        installed = true;
        SKSE::log::info("Staff attack gate installed (AttackBlockHandler::ProcessButton slot {:02X})", slot);
        return true;
    }
}
