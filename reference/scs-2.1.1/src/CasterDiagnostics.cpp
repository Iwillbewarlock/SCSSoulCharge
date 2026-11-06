#include "SpellCastingStaff/RuntimeCompatibility.h"
#include "SpellCastingStaff/CasterDiagnostics.h"
#include "SpellCastingStaff/Controller.h"
#include "SpellCastingStaff/PerkEntryProbe.h"
#include <RE/A/ActorMagicCaster.h>
#include <RE/A/ActiveEffect.h>
#include <RE/A/AIProcess.h>
#include <RE/H/HighProcessData.h>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <type_traits>
#include <utility>
#if defined(_WIN32)
#    include <Windows.h>
#endif

namespace SpellCastingStaff::CasterDiagnostics {
namespace {
    using Caster = RE::ActorMagicCaster;
    using EffectFn = void (*)(Caster*, RE::ActiveEffect*, float, bool);
    using PerksFn = void (*)(RE::ActiveEffect*, RE::Actor*, RE::MagicTarget*);
    using OnAddFn = void (*)(RE::ActiveEffect*, RE::MagicTarget*);
    using EvalFn = void (*)(RE::ActiveEffect*, float, bool);
    static_assert(std::is_same_v<decltype(&Caster::AdjustActiveEffect),
        void (Caster::*)(RE::ActiveEffect*, float, bool)>);
    static_assert(std::is_same_v<decltype(&RE::ActiveEffect::AdjustForPerks),
        void (RE::ActiveEffect::*)(RE::Actor*, RE::MagicTarget*)>);
    static_assert(std::is_same_v<decltype(&RE::ActiveEffect::OnAdd),
        void (RE::ActiveEffect::*)(RE::MagicTarget*)>);
    static_assert(std::is_same_v<decltype(&RE::ActiveEffect::EvaluateConditions),
        void (RE::ActiveEffect::*)(float, bool)>);
    REL::Relocation<EffectFn> effectOriginal;
    bool installed = false; // once, at kDataLoaded

    // A different thunk for every concrete Skyrim ActiveEffect vtable.
    // Each retains its own original function for correct forwarding.
    constexpr std::size_t kPerksTablesMax = 64;
    std::array<std::atomic<PerksFn>, kPerksTablesMax> perksOriginal{};
    std::array<std::atomic<OnAddFn>, kPerksTablesMax> addOriginal{};
    std::array<std::atomic<EvalFn>, kPerksTablesMax> evalOriginal{};
    std::array<std::uintptr_t, kPerksTablesMax> perksTables{};
    std::size_t perksTablesUsed = 0;
    std::mutex perksInstallMutex;
    std::atomic<bool> perkCapacityReported{false};
    std::array<std::atomic<bool>, kPerksTablesMax> chainWarningReported{};

    constexpr auto kFlagAV = RE::ActorValue::kVariable08;
    static_assert(static_cast<unsigned>(kFlagAV) == 75);
    using AVGetter = float (*)(RE::ActorValueOwner*, RE::ActorValue);
    static_assert(std::is_same_v<decltype(&RE::ActorValueOwner::GetActorValue),
        float (RE::ActorValueOwner::*)(RE::ActorValue)>);
    std::atomic<AVGetter> avOriginal{nullptr};
    std::atomic<std::uintptr_t> avHookVtable{0};
    std::mutex avInstallMutex;
    std::atomic<bool> baselineConflictReported{false};
    std::atomic<bool> missingGetterReported{false};

    // Only diagnostic counters; leave vtable lifetime/hook behavior unchanged.
    std::atomic<std::uint64_t> staffEffectSamples{0}, nonStaffEffectSamples{0};
    std::atomic<std::uint32_t> detailSamples{0};
    constexpr std::uint32_t kMaxDetailSamples = 32;

    // No persistent ownership map: active only inside a specific synchronous
    // CastSpellImmediate of a self-delivered BoundWeapon spell. OnAdd/Perks
    // may run within that call, before the summoned item unequips the staff.
    thread_local ScopedStaffBoundCast* activeStaffBoundCast = nullptr;

    // Zero modifications to the actual Actor Value. The override exists
    // only on the current thread for the duration of AdjustForPerks().
    struct VirtualAVScope;
    thread_local VirtualAVScope* activeAVScope = nullptr;
    struct VirtualAVScope {
        VirtualAVScope* parent = nullptr;
        RE::ActorValueOwner* owner = nullptr;
        float expected = 0.0f;
        bool armed = false;

        VirtualAVScope(RE::Actor* actor, bool sourceIsOther) {
            if (!actor || !avHookVtable.load(std::memory_order_acquire)) {
                if (!missingGetterReported.exchange(true))
                    SKSE::log::error("VAV08 UNAVAILABLE: getter hook not installed");
                return;
            }
            owner = actor->AsActorValueOwner();
            const auto getter = avOriginal.load(std::memory_order_acquire);
            if (!owner || !getter) {
                if (!missingGetterReported.exchange(true))
                    SKSE::log::error("VAV08 UNAVAILABLE: invalid AV owner/original");
                return;
            }
            const float base = owner->GetBaseActorValue(kFlagAV);
            const float total = getter(owner, kFlagAV); // true value, bypass virtual hook
            if (!std::isfinite(base) || !std::isfinite(total) ||
                (!activeAVScope && (base != 0.0f || total != 0.0f))) {
                if (!baselineConflictReported.exchange(true))
                    SKSE::log::error("VAV08 BASELINE CONFLICT: base={} total={}; staff perk gate skipped", base, total);
                return;
            }
            expected = sourceIsOther ? 1.0f : 0.0f;
            parent = activeAVScope;
            activeAVScope = this;
            armed = true;
        }
        ~VirtualAVScope() { if (armed) activeAVScope = parent; }
        VirtualAVScope(const VirtualAVScope&) = delete;
        VirtualAVScope& operator=(const VirtualAVScope&) = delete;
    };

    // The actor's high-process flag is NOT held from projectile launch to hit:
    // that would bleed into hand spells and other mods. Only enter while Skyrim
    // synchronously evaluates an ActiveEffect created by an owned DualCast staff.
    // Restore on every exit, including nested calls and early returns.
    struct ScopedActorDualEvaluation {
        RE::HighProcessData* high = nullptr;
        bool previous = false;
        bool armed = false;
        ScopedActorDualEvaluation(RE::ActiveEffect* effect) {
            // Only eligible, owned F&F effects get the dual bit / Impact bridge.
            // Concentration and BoundWeapon effects never enter this scope.
            if (!effect || !Controller::Get().OwnsDualCastEffect(effect)) return;
            // Skyrim's ActiveEffect carries its own dual-cast bit (1 << 12).
            // The user's CommonLibSSE-NG 3.6.0 lacks the named kDual enum,
            // so use the documented bit through the *existing* enum type.
            // This belongs to the individual effect, NOT the shared spell/MGEF.
            constexpr auto kDualEffectFlag = static_cast<RE::ActiveEffect::Flag>(1u << 12);
            const bool effectWasDual = effect->flags.all(kDualEffectFlag);
            if (!effectWasDual) effect->flags.set(kDualEffectFlag);
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* process = player ? player->GetActorRuntimeData().currentProcess : nullptr;
            high = process ? process->high : nullptr;
            if (!high) return;
            previous = high->isDualCasting;
            // If vanilla is already dual-casting, leave its state entirely alone.
            // We own only the false -> true -> false temporary transition.
            if (!previous) {
                high->isDualCasting = true;
                armed = true;
            }

        }
        ~ScopedActorDualEvaluation() {
            if (armed) high->isDualCasting = previous;
        }
        ScopedActorDualEvaluation(const ScopedActorDualEvaluation&) = delete;
        ScopedActorDualEvaluation& operator=(const ScopedActorDualEvaluation&) = delete;
    };

    float AVGetThunk(RE::ActorValueOwner* owner, RE::ActorValue value) {
        if (value == kFlagAV) {
            if (auto* scope = activeAVScope; scope && scope->armed && owner == scope->owner) {
                return scope->expected;
            }
        }
        // Another actor/thread, other AV, or no active effect: exact original.
        return avOriginal.load(std::memory_order_acquire)(owner, value);
    }

    bool InstallVirtualAVHook() {
        std::lock_guard lock(avInstallMutex);
        auto* pc = RE::PlayerCharacter::GetSingleton();
        auto* owner = pc ? pc->AsActorValueOwner() : nullptr;
        if (!owner) return false;
        const auto address = *reinterpret_cast<const std::uintptr_t*>(owner);
        if (!RuntimeCompatibility::ReadableMemory(address, 2 * sizeof(std::uintptr_t))) return false;
        const auto before = *(reinterpret_cast<const std::uintptr_t*>(address) + 0x01);
        const auto previous = avHookVtable.load(std::memory_order_acquire);
        if (previous) {
            if (previous != address) {
                SKSE::log::error("VAV08 getter vtable changed: prior={:X} now={:X} slotTarget={:X} module={}",
                    previous, address, before, RuntimeCompatibility::DescribeAddress(before));
                return false;
            }
            if (before == reinterpret_cast<std::uintptr_t>(&AVGetThunk)) return true;
            if (!RuntimeCompatibility::ExecutableTarget(before)) {
                SKSE::log::error("VAV08 getter replaced by invalid target={:X} module={}",
                    before, RuntimeCompatibility::DescribeAddress(before));
                return false;
            }
            // A later plugin can have chained our saved slot. Do NOT re-patch,
            // because that would either break the other plugin or create a cycle.
            // Executable target alone cannot PROVE that plugin calls AVGetThunk.
            static bool warnedLaterHook = false;
            if (!warnedLaterHook) {
                warnedLaterHook = true;
                SKSE::log::warn("VAV08 getter post-hook detected: slotTarget={:X} module={}; accepting shared vtable without re-patch; downstream forwarding NOT verified",
                    before, RuntimeCompatibility::DescribeAddress(before));
            }
            return true;
        }
#if defined(_WIN32)
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(address), &module) || module != GetModuleHandleW(nullptr)) {
            SKSE::log::error("VAV08 getter vtable not in Skyrim.exe; install rejected");
            return false;
        }
#endif
        if (!RuntimeCompatibility::ExecutableTarget(before) || before == reinterpret_cast<std::uintptr_t>(&AVGetThunk)) {
            SKSE::log::error("VAV08 getter rejected: target={:X} module={} (invalid or recursive)",
                before, RuntimeCompatibility::DescribeAddress(before));
            return false;
        }
        avOriginal.store(reinterpret_cast<AVGetter>(before), std::memory_order_release);
        REL::Relocation<std::uintptr_t> table{address};
        table.write_vfunc(0x01, reinterpret_cast<std::uintptr_t>(&AVGetThunk));
        avHookVtable.store(address, std::memory_order_release);
        SKSE::log::info("VAV08 getter installed");
        return true;
    }

    template<std::size_t Index>
    void PerksThunk(RE::ActiveEffect* effect, RE::Actor* actor, RE::MagicTarget* target) {
        const auto original = perksOriginal[Index].load(std::memory_order_acquire);
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc || actor != pc || !effect || !effect->spell || !PerkEntryProbe::Enabled()) {
            original(effect, actor, target);
            return;
        }
        // Normal hand spells need their real Variable08, unless nested inside
        // a kOther perk scope. Shadowing the outer scope prevents leakage.
        const bool nativeOther = effect->castingSource == RE::MagicSystem::CastingSource::kOther;
        const bool ourBoundHand = activeStaffBoundCast &&
                                  activeStaffBoundCast->Matches(effect, actor);
        const bool staffSource = nativeOther || ourBoundHand;
        // Swift and DualCast staffs have negative/compensating ESP PERKs meant
        // ONLY for eligible Fire-and-Forget spells. Do not feed those PERKs
        // Variable08=1 for Concentration or BoundWeapon effects.
        // For hand-bound summons, snapshot the staff kind BEFORE native equip
        // replacement; the weapon might already be unequipped by perk time.
        // ActiveEffect::spell is MagicItem*, while the staff's spell policy
        // intentionally takes SpellItem*. Do a checked form downcast rather
        // than passing the base pointer or forcing a reinterpret_cast.
        const auto* spellItem = effect->spell->As<RE::SpellItem>();
        const bool inverseExcluded = staffSource &&
            (Controller::Get().SuppressInversePerkForSpell(spellItem) ||
             (ourBoundHand && activeStaffBoundCast->InversePerkSuppressed()));
        const bool grantVirtualAV = staffSource && !inverseExcluded;
        const bool capture = PerkEntryProbe::Capturing();
        if (capture) {
            if (grantVirtualAV) ++staffEffectSamples;
            else ++nonStaffEffectSamples;
        }
        if (capture && inverseExcluded && detailSamples.fetch_add(1, std::memory_order_relaxed) < kMaxDetailSamples) {
            SKSE::log::warn("SCS17 INVERSE EXCLUDED spell={:08X} source={} bound={} (VAV08=0)",
                effect->spell->GetFormID(), static_cast<unsigned>(effect->castingSource), ourBoundHand);
        }
        if (!grantVirtualAV && !activeAVScope) {
            original(effect, actor, target);
            return;
        }
        ScopedActorDualEvaluation dualEffectScope(effect);
        const float durationBefore = ourBoundHand && grantVirtualAV ? effect->duration : 0.0f;
        const float magnitudeBefore = (capture && grantVirtualAV) ? effect->magnitude : 0.0f;
        const float diagnosticDurationBefore = (capture && grantVirtualAV) ? effect->duration : 0.0f;
        {
            VirtualAVScope gate(actor, grantVirtualAV);
            original(effect, actor, target); // ESP PERK decides the multiplier
        }
        if (capture && grantVirtualAV && detailSamples.fetch_add(1, std::memory_order_relaxed) < kMaxDetailSamples) {
            SKSE::log::warn("F6 PERK spell={:08X} source={} mag={:.3f}->{:.3f} duration={:.3f}->{:.3f} bound={}",
                effect->spell->GetFormID(), static_cast<unsigned>(effect->castingSource),
                magnitudeBefore, effect->magnitude, diagnosticDurationBefore, effect->duration, ourBoundHand);
        }
        if (ourBoundHand && grantVirtualAV) {
            SKSE::log::info("BOUND PERK spell={:08X} source={} duration={:.3f}->{:.3f} (native ESP PERK)",
                effect->spell->GetFormID(), static_cast<unsigned>(effect->castingSource),
                durationBefore, effect->duration);
        }
    }
    template<std::size_t... I>
    constexpr auto MakePerksThunks(std::index_sequence<I...>) {
        return std::array<PerksFn, sizeof...(I)>{&PerksThunk<I>...};
    }
    constexpr auto perksThunks = MakePerksThunks(std::make_index_sequence<kPerksTablesMax>{});

    template<std::size_t Index>
    void OnAddThunk(RE::ActiveEffect* effect, RE::MagicTarget* target) {
        ScopedActorDualEvaluation scope(effect);
        addOriginal[Index].load(std::memory_order_acquire)(effect, target);
    }
    template<std::size_t Index>
    void EvaluateThunk(RE::ActiveEffect* effect, float delta, bool force) {
        const auto original = evalOriginal[Index].load(std::memory_order_acquire);
        // Never touch persistent effect or actor dual bits for concentration.
        if (effect && effect->spell &&
            effect->spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
            original(effect, delta, force);
            return;
        }
        ScopedActorDualEvaluation scope(effect);
        original(effect, delta, force);
    }
    template<std::size_t... I>
    constexpr auto MakeOnAddThunks(std::index_sequence<I...>) {
        return std::array<OnAddFn, sizeof...(I)>{&OnAddThunk<I>...};
    }
    template<std::size_t... I>
    constexpr auto MakeEvalThunks(std::index_sequence<I...>) {
        return std::array<EvalFn, sizeof...(I)>{&EvaluateThunk<I>...};
    }
    constexpr auto addThunks = MakeOnAddThunks(std::make_index_sequence<kPerksTablesMax>{});
    constexpr auto evalThunks = MakeEvalThunks(std::make_index_sequence<kPerksTablesMax>{});

    bool InstallPerksForEffect(RE::ActiveEffect* effect) {
        if (!effect || !effect->spell) return false;
        const auto address = *reinterpret_cast<const std::uintptr_t*>(effect);
        if (!address) return false;
#if defined(_WIN32)
        // Only stable, executable-owned class tables. Never patch heap/shadow tables.
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(address), &module) || module != GetModuleHandleW(nullptr)) {
            return false;
        }
#endif
        std::lock_guard lock(perksInstallMutex);
        const auto entry = *reinterpret_cast<const std::uintptr_t*>(address);
        for (std::size_t i = 0; i < perksTablesUsed; ++i) {
            if (perksTables[i] != address) continue;
            const auto* tableSlots = reinterpret_cast<const std::uintptr_t*>(address);
            if (entry != reinterpret_cast<std::uintptr_t>(perksThunks[i]) ||
                tableSlots[0x01] != reinterpret_cast<std::uintptr_t>(addThunks[i]) ||
                tableSlots[0x05] != reinterpret_cast<std::uintptr_t>(evalThunks[i])) {
                if (!chainWarningReported[i].exchange(true))
                    SKSE::log::error("VAV08/DUAL effect class hook replaced (index={}); no unsafe re-patch", i);
            }
            return true;
        }
        if (!entry) {
            SKSE::log::error("VAV08 effect class has null AdjustForPerks function");
            return false;
        }
        if (perksTablesUsed == kPerksTablesMax) {
            if (!perkCapacityReported.exchange(true))
                SKSE::log::error("VAV08 effect vtable limit {} reached; new effects use original path", kPerksTablesMax);
            return false;
        }
        for (const auto thunk : perksThunks) {
            if (entry == reinterpret_cast<std::uintptr_t>(thunk)) {
                SKSE::log::error("VAV08 unexpected duplicate thunk; not patching");
                return false;
            }
        }
        const auto* slots = reinterpret_cast<const std::uintptr_t*>(address);
        const auto oldAdd = slots[0x01];
        const auto oldEval = slots[0x05];
        if (!oldAdd || !oldEval) {
            SKSE::log::error("DUAL effect class missing OnAdd/EvaluateConditions; no hooks installed");
            return false;
        }
        for (std::size_t i = 0; i < kPerksTablesMax; ++i) {
            if (oldAdd == reinterpret_cast<std::uintptr_t>(addThunks[i]) ||
                oldEval == reinterpret_cast<std::uintptr_t>(evalThunks[i])) {
                SKSE::log::error("DUAL effect class unexpected recursive thunk; no hooks installed");
                return false;
            }
        }
        const auto index = perksTablesUsed;
        perksTables[index] = address;
        perksOriginal[index].store(reinterpret_cast<PerksFn>(entry), std::memory_order_release);
        addOriginal[index].store(reinterpret_cast<OnAddFn>(oldAdd), std::memory_order_release);
        evalOriginal[index].store(reinterpret_cast<EvalFn>(oldEval), std::memory_order_release);
        REL::Relocation<std::uintptr_t> table{address};
        table.write_vfunc(0x01, reinterpret_cast<std::uintptr_t>(addThunks[index]));
        table.write_vfunc(0x05, reinterpret_cast<std::uintptr_t>(evalThunks[index]));
        table.write_vfunc(0x00, reinterpret_cast<std::uintptr_t>(perksThunks[index]));
        ++perksTablesUsed;
        SKSE::log::info("VAV08 effect class registered: index={} spell={:08X}",
                        index, effect->spell->GetFormID());
        return true;
    }

    void EffectThunk(Caster* caster, RE::ActiveEffect* effect, float power, bool arg3) {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (pc && caster && caster->actor == pc && PerkEntryProbe::Enabled() && effect && effect->spell)
            InstallPerksForEffect(effect);
        ScopedActorDualEvaluation scope(effect);
        effectOriginal(caster, effect, power, arg3);
    }
}

ScopedStaffBoundCast::ScopedStaffBoundCast(RE::PlayerCharacter* a_player, RE::SpellItem* a_spell,
                                               RE::MagicSystem::CastingSource a_source)
    : player_(a_player), spell_(a_spell), source_(a_source),
      inversePerkSuppressed_(Controller::Get().SuppressInversePerkForSpell(a_spell)) {
    if (!player_ || !spell_ || a_source == RE::MagicSystem::CastingSource::kOther) return;
    previous_ = activeStaffBoundCast;
    activeStaffBoundCast = this;
    armed_ = true;
}

ScopedStaffBoundCast::~ScopedStaffBoundCast() {
    if (armed_) activeStaffBoundCast = previous_;
}

bool ScopedStaffBoundCast::Matches(const RE::ActiveEffect* a_effect, const RE::Actor* a_actor) const {
    return armed_ && a_effect && a_actor == player_ &&
           a_effect->spell == spell_ && a_effect->castingSource == source_;
}

bool Install() {
    if (installed) return true;
    // This is the ONLY ActorMagicCaster hook; CheckCast and InterruptCast
    // are NOT patched. PlayerCharacter perk-query hooks are NOT patched.
    REL::Relocation<std::uintptr_t> table{RE::VTABLE_ActorMagicCaster[0]};
    if (!RuntimeCompatibility::ReadableMemory(table.address() + 0x1C * sizeof(void*), sizeof(std::uintptr_t))) return false;
    const auto original = *reinterpret_cast<const std::uintptr_t*>(table.address() + 0x1C * sizeof(void*));
    if (!RuntimeCompatibility::ExecutableTarget(original) || original == reinterpret_cast<std::uintptr_t>(&EffectThunk)) {
        SKSE::log::error("VAV08 effect dispatch rejected: target={:X} module={} (invalid/recursive)",
            original, RuntimeCompatibility::DescribeAddress(original));
        return false;
    }
    effectOriginal = table.write_vfunc(0x1C, reinterpret_cast<std::uintptr_t>(&EffectThunk));
    installed = true;
    SKSE::log::info("VAV08 engine effect hook ready (AdjustActiveEffect slot 0x1C)");
    return true;
}

bool ArmVirtualAVHook() {
    if (!installed) return false;
    return InstallVirtualAVHook();
}

void Reset() {
    staffEffectSamples.store(0, std::memory_order_relaxed);
    nonStaffEffectSamples.store(0, std::memory_order_relaxed);
    detailSamples.store(0, std::memory_order_relaxed);
}
void Summary() {
    SKSE::log::warn("F6 PERK SUMMARY staffEffects={} otherEffects={} samplesLogged={}/{} classes={}/{}",
        staffEffectSamples.load(std::memory_order_relaxed),
        nonStaffEffectSamples.load(std::memory_order_relaxed),
        std::min(detailSamples.load(std::memory_order_relaxed), kMaxDetailSamples),
        kMaxDetailSamples, perksTablesUsed, kPerksTablesMax);
}

} // namespace SpellCastingStaff::CasterDiagnostics
