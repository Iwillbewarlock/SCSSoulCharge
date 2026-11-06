// SE 1.5.97 / Steam 1.6.1170. See PORT_AUDIT.md.
// The validated BoundItemEffect::Finish() native call at Finish+0x112 passes
// r8 = &this->restorationData (+0x98). Wrap that call to identify the EXACT
// finishing ActiveEffect; then modify the equip slot ONLY if that effect came
// from the left hand. This prevents a right-hand Finish from borrowing the
// existence of some unrelated left-hand bound effect as evidence.
//
// No manual re-equips, dispels, timers, deferred casting, or global vtable hooks.
// Both native CALL sites and their targets are verified BEFORE any patch.
#include "SpellCastingStaff/BoundLifecycleHook.h"
#include "SpellCastingStaff/RuntimeCompatibility.h"
#include "SpellCastingStaff/BoundRestorePolicy.h"
#include "SpellCastingStaff/Controller.h"
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <Windows.h>
#include <spdlog/spdlog.h>

namespace SpellCastingStaff::BoundLifecycleHook {
namespace {
    using EquipFn = void (*)(RE::ActorEquipManager*, RE::Actor*, RE::TESBoundObject*,
        RE::ExtraDataList*, std::uint32_t, const RE::BGSEquipSlot*, bool, bool, bool, bool);
    // Native function 0x...F485D0: (equip manager, actor, address of
    // BoundItemEffect restorationData). Return is ignored by Finish().
    using FinishRestoreFn = void (*)(RE::ActorEquipManager*, RE::Actor*, void*);

    EquipFn originalEquip{};
    FinishRestoreFn originalFinishRestore{};
    bool installed{};
    RE::BGSKeyword* ownedKeyword{};
    const RE::BGSEquipSlot* leftEquipSlot{};
    thread_local bool processing{};
    // Only active inside the verified Finish->restore-helper native call.
    // Scoped, per-thread and restored across nested calls. NOT a global
    // historical cast record (which would be stale after save/load).
    thread_local bool finishingVerifiedLeftBoundEffect{};

    struct ScopedFlag {
        ScopedFlag(bool& storage, bool newValue) : flag(storage), previous(storage) {
            flag = newValue;
        }
        ~ScopedFlag() { flag = previous; }
        ScopedFlag(const ScopedFlag&) = delete;
        ScopedFlag& operator=(const ScopedFlag&) = delete;
    private:
        bool& flag;
        bool previous;
    };

    [[nodiscard]] bool DecodeNearCall(std::uintptr_t site, std::uintptr_t& target) {
        // The optional installer must be self-contained; the global preflight
        // no longer guarantees these CALL-site pages are readable.
        if (!RuntimeCompatibility::ReadableMemory(site, 5) ||
            *reinterpret_cast<const std::uint8_t*>(site) != 0xE8) return false;
        std::int32_t displacement = 0;
        std::memcpy(&displacement, reinterpret_cast<const void*>(site + 1), sizeof(displacement));
        target = site + 5 + static_cast<std::intptr_t>(displacement);
        return RuntimeCompatibility::ExecutableTarget(target);
    }

    // Best-effort transaction rollback if the second trampoline write throws.
    // Restores the ORIGINAL E8 bytes, not a guessed CALL destination, so it
    // leaves any future reattempt (or native behavior) as it was on entry.
    [[nodiscard]] bool RestoreCall(std::uintptr_t site,
                                   const std::array<std::uint8_t, 5>& bytes) {
        DWORD previous{};
        auto* destination = reinterpret_cast<void*>(site);
        if (!VirtualProtect(destination, bytes.size(), PAGE_EXECUTE_READWRITE, &previous))
            return false;
        std::memcpy(destination, bytes.data(), bytes.size());
        FlushInstructionCache(GetCurrentProcess(), destination, bytes.size());
        DWORD ignored{};
        return VirtualProtect(destination, bytes.size(), previous, &ignored) != 0;
    }

    [[nodiscard]] bool OurStaff(const RE::TESBoundObject* object) {
        const auto* weapon = object ? object->As<RE::TESObjectWEAP>() : nullptr;
        return weapon && ownedKeyword && weapon->HasKeyword(ownedKeyword);
    }

    // Do not dereference a synthetic candidate merely because it looks like
    // an ActiveEffect pointer. First locate the exact address in the player's
    // engine-owned active effect list (bounded walk), THEN inspect its data.
    [[nodiscard]] bool IsFinishingLeftBoundEffect(RE::PlayerCharacter* player,
                                                  const void* restorationData) {
        if (!player || !restorationData) return false;
        constexpr std::uintptr_t kRestorationDataOffset = 0x98; // x64dbg, SE 1.5.97
        const auto addr = reinterpret_cast<std::uintptr_t>(restorationData);
        if (addr < kRestorationDataOffset) return false;
        auto* candidate = reinterpret_cast<RE::ActiveEffect*>(addr - kRestorationDataOffset);
        auto* target = player->AsMagicTarget();
        auto* list = target ? target->GetActiveEffectList() : nullptr;
        if (!list) return false;
        std::size_t inspected = 0;
        for (auto* effect : *list) {
            if (++inspected > 512) break;
            if (effect != candidate) continue;
            // The bound effect may already be marked dispelled during Finish;
            // do not reject it on that flag alone.
            if (!effect || effect->target != target ||
                effect->castingSource != RE::MagicSystem::CastingSource::kLeftHand) {
                return false;
            }
            const auto* base = effect->GetBaseObject();
            return base && base->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kBoundWeapon;
        }
        return false; // detached/unexpected effect => forward vanilla arguments
    }

    // Hook of the exact 5-byte CALL in BoundItemEffect::Finish. Its third
    // argument is lea r8,[rdi+98], where rdi=this (confirmed disassembly).
    // Reading the finishing effect HERE, not at EquipObject, is what makes
    // the actual hand distinguishable when BOTH hands have bound weapons.
    void FinishRestoreThunk(RE::ActorEquipManager* manager, RE::Actor* actor,
                            void* restorationData) {
        if (!originalFinishRestore) return; // not reachable after installation
        auto* pc = RE::PlayerCharacter::GetSingleton();
        const bool verifiedLeft = Controller::Get().LifecycleReady() &&
                                  manager && pc && actor == pc &&
                                  IsFinishingLeftBoundEffect(pc, restorationData);
        ScopedFlag scope{finishingVerifiedLeftBoundEffect, verifiedLeft};
        originalFinishRestore(manager, actor, restorationData);
    }

    // Hook of the native helper's CALL to ActorEquipManager::EquipObject.
    // All inputs forwarded byte-for-byte except slot on the verified path.
    void EquipThunk(RE::ActorEquipManager* manager, RE::Actor* actor,
                   RE::TESBoundObject* object, RE::ExtraDataList* extra,
                   std::uint32_t count, const RE::BGSEquipSlot* slot,
                   bool queueEquip, bool forceEquip, bool playSounds, bool applyNow) {
        if (!originalEquip) return;
        auto* pc = RE::PlayerCharacter::GetSingleton();
        const bool cheapChecks = Controller::Get().LifecycleReady() &&
            !processing && manager && pc && actor == pc &&
            count == 1 && slot == nullptr && leftEquipSlot && OurStaff(object);
        if (cheapChecks) {
            auto* leftObject = pc->GetEquippedObject(true);
            auto* leftWeapon = leftObject ? leftObject->As<RE::TESObjectWEAP>() : nullptr;
            const BoundRestorePolicy::Evidence evidence{
                .verifiedFinishingLeftBound = finishingVerifiedLeftBoundEffect,
                .leftHandCurrentlyBound = leftWeapon && leftWeapon->IsBound(),
                .restoredItemIsOurStaff = true,
                .slotUnspecified = true,
                .countOne = true
            };
            if (BoundRestorePolicy::ShouldCorrect(evidence)) {
                ScopedFlag reentry{processing, true};
                originalEquip(manager, actor, object, extra, count, leftEquipSlot,
                              queueEquip, forceEquip, playSounds, applyNow);
                SKSE::log::info("BOUND RESTORE corrected verifiedFinish=LEFT staff={:08X} left={:08X} right={:08X}",
                    object->GetFormID(),
                    pc->GetEquippedObject(true) ? pc->GetEquippedObject(true)->GetFormID() : 0u,
                    pc->GetEquippedObject(false) ? pc->GetEquippedObject(false)->GetFormID() : 0u);
                return;
            }
            // TEST build: a visible bypass is useful when verifying that a
            // RIGHT Finish does not consume another LEFT bound effect.
            SKSE::log::info("BOUND RESTORE bypass: Finish effect not verified left (staff={:08X})",
                            object->GetFormID());
        }
        originalEquip(manager, actor, object, extra, count, slot,
                      queueEquip, forceEquip, playSounds, applyNow);
    }
} // namespace

bool Install() {
    if (installed) return true;
    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) {
        SKSE::log::error("BOUND RESTORE unavailable: TESDataHandler missing");
        return false;
    }
    leftEquipSlot = handler->LookupForm<RE::BGSEquipSlot>(0x13F43, "Skyrim.esm");
    if (!leftEquipSlot) {
        SKSE::log::error("BOUND RESTORE unavailable: Skyrim.esm LeftHand slot missing");
        return false;
    }
    ownedKeyword = handler->LookupForm<RE::BGSKeyword>(kKeywordID, kPluginName);
    if (!ownedKeyword) {
        SKSE::log::error("BOUND RESTORE unavailable: common staff keyword missing");
        return false;
    }

    const REL::Relocation<std::uintptr_t> equip{RE::Offset::ActorEquipManager::EquipObject};
    const bool ae = RuntimeCompatibility::IsSupportedAE();
    const auto restoreHelper = ae ? REL::ID(38906).address() : equip.address() + 0xB50;
    const auto equipCall = ae ? restoreHelper + 0xFD : equip.address() + 0xC41;
    const auto finishCall = ae ? REL::ID(34227).address() + 0x10D : equip.address() - 0xF1DEE;
    std::uintptr_t actualEquipTarget{};
    std::uintptr_t actualRestoreTarget{};
    // Also assert `lea r8,[rdi+0x98]` immediately before Finish's CALL.
    // Without this exact instruction the incoming data pointer must NOT be
    // translated back to an ActiveEffect address.
    constexpr std::array<std::uint8_t, 7> kFinishLea{
        0x4C, 0x8D, 0x87, 0x98, 0x00, 0x00, 0x00
    };
    const auto leaAddress = finishCall - kFinishLea.size();
    const bool correctEffectPointerProvenance =
        RuntimeCompatibility::ReadableMemory(leaAddress, kFinishLea.size()) &&
        std::memcmp(reinterpret_cast<const void*>(leaAddress),
                    kFinishLea.data(), kFinishLea.size()) == 0;
    if (!DecodeNearCall(equipCall, actualEquipTarget) || actualEquipTarget != equip.address() ||
        !DecodeNearCall(finishCall, actualRestoreTarget) || actualRestoreTarget != restoreHelper ||
        !correctEffectPointerProvenance) {
        SKSE::log::error("BOUND RESTORE not installed: native CALL target mismatch "
                         "equipCall={:X} got={:X} expected={:X}; FinishCall={:X} got={:X} expected={:X}; leaMatch={}",
                         equipCall, actualEquipTarget, equip.address(),
                         finishCall, actualRestoreTarget, restoreHelper, correctEffectPointerProvenance);
        SKSE::log::error("BOUND RESTORE target modules: equip={} finish={} (MEM_PRIVATE owner not inferable)",
            RuntimeCompatibility::DescribeAddress(actualEquipTarget),
            RuntimeCompatibility::DescribeAddress(actualRestoreTarget));
        return false;
    }

    // Both CALLs and the LEA have passed the COMPLETE check before ANY write.
    // Capture original bytes for rollback rather than leaving a half-installed
    // optional feature if allocating a branch stub fails on the second CALL.
    std::array<std::uint8_t, 5> equipBytes{};
    std::array<std::uint8_t, 5> finishBytes{};
    std::memcpy(equipBytes.data(), reinterpret_cast<const void*>(equipCall), equipBytes.size());
    std::memcpy(finishBytes.data(), reinterpret_cast<const void*>(finishCall), finishBytes.size());

    // Initialize original targets BEFORE the hooks become reachable. All
    // original targets were verified as native, executable functions above.
    originalEquip = reinterpret_cast<EquipFn>(actualEquipTarget);
    originalFinishRestore = reinterpret_cast<FinishRestoreFn>(actualRestoreTarget);
    try {
        // 128-byte shared trampoline is allocated ONCE in Plugin.cpp.
        auto& trampoline = SKSE::GetTrampoline();
        trampoline.write_call<5>(equipCall, &EquipThunk);
        trampoline.write_call<5>(finishCall, &FinishRestoreThunk);
    } catch (const std::exception& e) {
        const bool restoredFinish = RestoreCall(finishCall, finishBytes);
        const bool restoredEquip = RestoreCall(equipCall, equipBytes);
        SKSE::log::critical("BOUND RESTORE patch exception: {}; rollback finish={} equip={}",
                            e.what(), restoredFinish, restoredEquip);
        if (restoredFinish && restoredEquip) {
            originalEquip = nullptr;
            originalFinishRestore = nullptr;
        }
        return false;
    } catch (...) {
        const bool restoredFinish = RestoreCall(finishCall, finishBytes);
        const bool restoredEquip = RestoreCall(equipCall, equipBytes);
        SKSE::log::critical("BOUND RESTORE unknown patch exception; rollback finish={} equip={}",
                            restoredFinish, restoredEquip);
        if (restoredFinish && restoredEquip) {
            originalEquip = nullptr;
            originalFinishRestore = nullptr;
        }
        return false;
    }
    installed = true;
    SKSE::log::info("BOUND RESTORE instance-scoped hook installed (SE/AE1170, 2 verified CALL sites)");
    return true;
}
} // namespace SpellCastingStaff::BoundLifecycleHook
