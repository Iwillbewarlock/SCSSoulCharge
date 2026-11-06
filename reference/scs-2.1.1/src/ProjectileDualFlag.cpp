// Native F&F dual projectile metadata. No concentration arm or diagnostic sampling.
// Main-thread-only scan on the existing frame hook, bounded by handle/age/time.
#include "SpellCastingStaff/ProjectileDualFlag.h"
#include "SpellCastingStaff/Controller.h"
#include "SpellCastingStaff/Config.h"
#include <RE/P/Projectile.h>
#include <RE/B/BSAtomic.h>
#include <array>
#include <chrono>
#include <cstdint>

namespace SpellCastingStaff::ProjectileDualFlag {
namespace {
    using Clock = std::chrono::steady_clock;
    constexpr auto kArmWindow = std::chrono::milliseconds(700);
    constexpr float kMaxProjectileAge = 0.35f;
    constexpr std::size_t kHandleBudget = 128;
    constexpr std::size_t kSeenBudget = 64;
    // CommonLibSSE-NG 3.6.0 projectile data stores flags as uint32_t.
    constexpr std::uint32_t kProjectileIsDualBit = std::uint32_t{1} << 28;
    struct Pending {
        RE::PlayerCharacter* player{};
        RE::SpellItem* spell{};
        Clock::time_point expires{};
    } pending;
    std::array<std::uint32_t, kSeenBudget> seen{};
    std::size_t seenCount = 0;
    bool WasSeen(std::uint32_t handle) {
        for (std::size_t i = 0; i < seenCount; ++i)
            if (seen[i] == handle) return true;
        return false;
    }
    void Remember(std::uint32_t handle) {
        if (!handle) return;
        if (seenCount >= seen.size()) seenCount = 0;
        seen[seenCount++] = handle;
    }
}
void Reset() {
    // Never rewrite previously launched projectiles on load/world transition.
    pending = {};
    seenCount = 0;
}
void Arm(RE::PlayerCharacter* player, RE::SpellItem* spell) {
    if (!GetConfig().experimentalProjectileDual || !player || !spell ||
        !Controller::Get().OwnsDualCastSpell(player, spell, RE::MagicSystem::CastingSource::kOther)) return;
    pending = {player, spell, Clock::now() + kArmWindow};
}
void Update() {
    // No world scan at all without a recent owned F&F cast.
    if (!pending.spell) return;
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || player->IsDead()) { Reset(); return; }
    if (Clock::now() >= pending.expires || player != pending.player ||
        !Controller::Get().OwnsDualCastSpell(player, pending.spell,
            RE::MagicSystem::CastingSource::kOther)) {
        pending = {};
        return;
    }
    auto* manager = RE::Projectile::Manager::GetSingleton();
    if (!manager) return;
    std::array<RE::ProjectileHandle, kHandleBudget> handles{};
    std::size_t n = 0;
    {
        RE::BSSpinLockGuard guard(manager->projectileLock);
        const auto copy = [&](const auto& list) {
            for (const auto& h : list) {
                if (n == handles.size()) return;
                handles[n++] = h;
            }
        };
        copy(manager->unlimited);
        copy(manager->limited);
        copy(manager->pending);
    }
    for (std::size_t i = 0; i < n; ++i) {
        const auto handleID = handles[i].native_handle();
        if (!handleID || WasSeen(handleID)) continue;
        auto projectile = handles[i].get();
        if (!projectile) continue;
        auto& data = projectile->GetProjectileRuntimeData();
        if (!data.spell) continue;
        if (data.castingSource != RE::MagicSystem::CastingSource::kOther &&
            data.castingSource != RE::MagicSystem::CastingSource::kLeftHand &&
            data.castingSource != RE::MagicSystem::CastingSource::kRightHand) continue;
        auto shooter = data.shooter.get();
        if (!shooter || shooter.get() != player) continue;
        const bool owned = pending.spell && data.spell == pending.spell &&
                           data.castingSource == RE::MagicSystem::CastingSource::kOther;
        const bool young = data.livingTime >= 0.0f && data.livingTime <= kMaxProjectileAge;
        if (owned && young) data.flags |= kProjectileIsDualBit;
        Remember(handleID);
    }
}
} // namespace SpellCastingStaff::ProjectileDualFlag
