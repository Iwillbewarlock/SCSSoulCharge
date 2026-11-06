// v0.4.7 split from v0.4.6 plugin.cpp, without intentional logic changes.
#include "SpellCastingStaff/Controller.h"
#include "SpellCastingStaff/PerkEntryProbe.h"
#include <spdlog/spdlog.h>
#include "SpellCastingStaff/ReviewPolicy.h"

namespace SpellCastingStaff {

    RE::BSEventNotifyControl Controller::ProcessEvent(const RE::TESEquipEvent* event,
                                          RE::BSTEventSource<RE::TESEquipEvent>*) {
        if (enabled_ && gameReady_ && event && event->actor && event->actor->IsPlayerRef()) {
            auto* form = RE::TESForm::LookupByID(event->baseObject);
            if (event->equipped && IsStaff(form)) {
                preferredStaff_ = event->baseObject;
            }
            if (IsStaff(form)) {
                inventoryNamePreviewPending_.store(true, std::memory_order_release);
            }
            // Defer until the equipment transaction finishes.
            QueueSync();
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl Controller::ProcessEvent(const RE::BSAnimationGraphEvent* event,
                                          RE::BSTEventSource<RE::BSAnimationGraphEvent>*) {
        if (!enabled_ || !gameReady_ || !event || !event->holder || !event->holder->IsPlayerRef()) {
            return RE::BSEventNotifyControl::kContinue;
        }
        const char* tagText = event->tag.c_str();
        if (!tagText) { return RE::BSEventNotifyControl::kContinue; }
        const std::string_view tag{tagText};
        // Synchronous native interruption of our exact left-hand dummy ENCH
        // is a handoff, NOT a cancelled spell. Do not refund the already-paid
        // Stamina escrow or block the same press during this narrow scope.
        if (boundLeftDummyHandoff_ && (tag == "Spell_Stop" || tag == "Spell_Interrupt" ||
            tag == "InterruptCast" || tag == "MLh_SpellStop_Event" ||
            tag == "MLh_SpellInterrupt_Event")) {
            return RE::BSEventNotifyControl::kContinue;
        }
        if (tag == "MLh_SpellFire_Event") {
            Fire(true);
        } else if (tag == "MRh_SpellFire_Event") {
            Fire(false);
        } else if (tag == "Spell_Stop" || tag == "Spell_Interrupt" || tag == "InterruptCast" ||
                   tag == "weapSheathe" || tag == "MRh_SpellStop_Event" ||
                   tag == "MLh_SpellStop_Event" || tag == "MRh_SpellInterrupt_Event" ||
                   tag == "MLh_SpellInterrupt_Event") {
            // Stop tags can be part of a NORMAL release-to-fire sequence.
            // Settle those using native caster ownership/idle on the frame hook.
            // Explicit interruption/holstering is unambiguously cancellation.
            if (tag == "Spell_Interrupt" || tag == "InterruptCast" || tag == "weapSheathe" ||
                tag == "MRh_SpellInterrupt_Event" || tag == "MLh_SpellInterrupt_Event") {
                if (tag != "MRh_SpellInterrupt_Event") CancelCharge(true, "animation interrupt");
                if (tag != "MLh_SpellInterrupt_Event") CancelCharge(false, "animation interrupt");
            }
            // Generic interrupt/holster events are cancellation points.
            const auto stop = AnimationStopPolicy(tag);
            if (stop.left) {
                StopChannel(true, "animation stop");
                StopRapid(true, "native animation stop event");
            }
            if (stop.right) {
                StopChannel(false, "animation stop");
                StopRapid(false, "native animation stop event");
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl Controller::ProcessEvent(RE::InputEvent* const* events,
                                          RE::BSTEventSource<RE::InputEvent*>*) {
        if (!enabled_ || !lifecycleReady_.load(std::memory_order_acquire) || !events) {
            return RE::BSEventNotifyControl::kContinue;
        }
        for (auto* event = *events; event; event = event->next) {
            if (event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) {
                continue;
            }
            auto* button = event->AsButtonEvent();
            if (!button) {
                continue;
            }
            PerkEntryProbe::OnButtonEvent(button); // F6: HUD notice + bounded diagnostics, never consumes input.
            // Engine input sinks can execute on a different thread from
            // equipment Sync(). Never change Controller casting state here.
            const auto* userEvent = button->QUserEvent().c_str();
            if (!userEvent) continue;
            const auto eventName = std::string_view{userEvent};
            if (eventName == "Left Attack" || eventName == "Left Attack/Block") {
                StageInputSinkAttack(0, button->Value() > 0.0f);
            } else if (eventName == "Right Attack" || eventName == "Right Attack/Block") {
                StageInputSinkAttack(1, button->Value() > 0.0f);
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl Controller::ProcessEvent(const RE::MenuOpenCloseEvent* event,
                                          RE::BSTEventSource<RE::MenuOpenCloseEvent>*) {
        if (enabled_ && gameReady_ && event && event->opening) {
            const char* menuText = event->menuName.c_str();
            if (!menuText) { return RE::BSEventNotifyControl::kContinue; }
            const std::string_view menu{menuText};
            // HUD and dialogue overlays do not necessarily interrupt a cast.
            if (menu != "HUD Menu" && menu != "Dialogue Menu") {
                if (charges_[0].active || charges_[1].active ||
                    channels_[0].running || channels_[1].running ||
                    rapid_[0].running || rapid_[1].running) {
                    SKSE::log::info("CAST cancelled by menu: {}", menu);
                }
                CancelCharge(true, "menu opened");
                CancelCharge(false, "menu opened");
                StopChannel(true, "menu opened");
                StopChannel(false, "menu opened");
                StopRapid(true, "menu opened");
                StopRapid(false, "menu opened");
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }

} // namespace SpellCastingStaff
