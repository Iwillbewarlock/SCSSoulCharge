#include "SpellCastingStaff/Controller.h"
#include "SpellCastingStaff/InputSnapshotPolicy.h"
#include "SpellCastingStaff/Config.h"
#include "SpellCastingStaff/DualCastCost.h"
#include <spdlog/spdlog.h>
namespace SpellCastingStaff {
namespace {
    int AttackIndex(RE::ButtonEvent* button) {
        if (!button) return -1;
        const char* actionText = button->QUserEvent().c_str();
        if (!actionText) return -1;
        const std::string_view action{actionText};
        if (action == "Left Attack" || action == "Left Attack/Block") return 0;
        if (action == "Right Attack" || action == "Right Attack/Block") return 1;
        return -1;
    }
}

void Controller::ObserveAttackState(std::size_t idx, bool held) {
    const bool wasHeld = attackHeld_[idx];
    attackHeld_[idx] = held;
    attackStateKnown_[idx] = true;
    if (held && !wasHeld) {
        StopRapid(idx == 0, "new attack press");
        firedThisPress_[idx] = false;
        attackPressedAt_[idx] = std::chrono::steady_clock::now();
    }
    if (!held) {
        gateHeld_[idx] = false; // release may be observed only by the sink in menus
        StopChannel(idx == 0, "attack input released");
        StopRapid(idx == 0, "attack input released");
        SyncStaminaCastingGlobal();
    }
    // F&F release is NOT cancellation: SpellFire may still follow.
}

void Controller::ObserveAttack(RE::ButtonEvent* button) {
    const int index = AttackIndex(button);
    if (!enabled_ || !gameReady_ || index < 0) return;
    const auto idx = static_cast<std::size_t>(index);
    // The native hook is authoritative. Invalidate any older sink snapshot;
    // this never postpones a staff preflight or a native release.
    nativeAttackSequence_[idx].fetch_add(1, std::memory_order_acq_rel);
    ObserveAttackState(idx, button->Value() > 0.0f);
}

void Controller::StageInputSinkAttack(std::size_t idx, bool held) {
    if (idx > 1) return;
    const auto nativeSeq = nativeAttackSequence_[idx].load(std::memory_order_acquire);
    // Zero means no event. Bits: native sequence in the upper bits,
    // 1 = released, 2 = pressed (last observed event wins).
    stagedInput_[idx].store(InputSnapshotPolicy::Pack(nativeSeq, held), std::memory_order_release);
}

void Controller::DrainStagedInput() {
    for (std::size_t idx = 0; idx != stagedInput_.size(); ++idx) {
        const auto value = stagedInput_[idx].exchange(0, std::memory_order_acq_rel);
        if (!value) continue;
        bool pressed = false;
        if (!InputSnapshotPolicy::Read(value, nativeAttackSequence_[idx].load(std::memory_order_acquire), pressed)) continue;
        ObserveAttackState(idx, pressed);
    }
}

bool Controller::AllowStaffAttack(RE::ButtonEvent* button) {
    const int index = AttackIndex(button);
    if (index < 0) return true;
    if (HasCompatibilityFailure()) {
        // Never permit an unbound dummy staff to attack when menu assignment is unavailable.
        // Releases still reach the engine so native input state cannot stick.
        if (button->Value() <= 0.0f) return true;
        if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
            return Tier(pc->GetEquippedObject(index == 0)) < 0;
        }
        return false;
    }
    if (!enabled_ || !gameReady_) return true;
    const auto idx = static_cast<std::size_t>(index);
    const bool left = idx == 0;
    ObserveAttack(button); // independent of input sink ordering, idempotent
    const bool held = button->Value() > 0.0f;
    if (!held) {
        gateHeld_[idx] = false;
        drawOnlyPress_[idx] = false;
        return true; // ALWAYS forward release, even for rejected presses
    }
    auto* pc = RE::PlayerCharacter::GetSingleton();
    if (!pc || Tier(pc->GetEquippedObject(left)) < 0) {
        gateHeld_[idx] = false;
        gateBlocked_[idx] = false;
        return true; // ordinary weapons, magic and staffs are never gated
    }
    // Fresh games can begin before the player's animation graph is ready.
    // Attach before the very first native staff attack so its SpellFire tag
    // cannot be missed even if the periodic frame check has not run yet.
    if (!gateHeld_[idx] && !drawOnlyPress_[idx]) {
        RegisterPlayerAnimationSink();
    }
    // A single physical press that drew the staff MUST NOT later start
    // charging on repeated held callbacks after the weapon becomes drawn.
    const auto* actorState = pc->AsActorState();
    if (!actorState) return true; // leave input to vanilla if actor state is unavailable
    const bool weaponDrawn = actorState->IsWeaponDrawn();
    if (drawOnlyPress_[idx]) return !weaponDrawn;
    // Forward the first press while sheathed to the vanilla attack handler:
    // it is a DRAW command, not an attempt to cast. Never start our ledger.
    if (!weaponDrawn) {
        drawOnlyPress_[idx] = true;
        gateHeld_[idx] = false;
        gateBlocked_[idx] = false;
        return true;
    }
    if (gateHeld_[idx]) return !gateBlocked_[idx];
    gateHeld_[idx] = true;
    gateBlocked_[idx] = false;
    CancelCharge(left, "new attack press");
    gateBlocked_[idx] = false;
    firedThisPress_[idx] = false;
    const auto reject = [&](std::string_view reason) {
        gateBlocked_[idx] = true;
        firedThisPress_[idx] = true;
        SKSE::log::info("CHARGE {} BLOCK {}", left ? "L" : "R", reason);
        return false;
    };
    const auto& hand = hands_[idx];
    auto* equipped = pc->GetEquippedObject(left);
    auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(hand.spell);
    if (!hand.spell && equipped && hand.staff == equipped->GetFormID() && !pc->IsDead()) {
        // One notification per accepted DOWN, additionally rate-limited across hands.
        const auto now = std::chrono::steady_clock::now();
        const auto& config = GetConfig();
        if (config.showMissingSpellMessage && !config.missingSpellMessage.empty() &&
            (lastMissingSpellNotice_ == std::chrono::steady_clock::time_point{} ||
             now - lastMissingSpellNotice_ >= std::chrono::seconds(2))) {
            lastMissingSpellNotice_ = now;
            RE::DebugNotification(config.missingSpellMessage.c_str());
        }
        return reject("no remembered spell");
    }
    if (!frameHookReady_ || !enchantment_ || pc->IsDead() || !equipped || hand.staff != equipped->GetFormID() ||
        IsStaff(pc->GetEquippedObject(!left)) || !ValidSpell(pc, spell, equipped)) {
        return reject("binding or frame lifecycle unavailable");
    }
    // Do not begin charging against stale shared ENCH metadata.
    SyncNativeEnchantmentProperties(pc);
    if (nativeEnchantmentSyncPending_) return reject("native ENCH sync pending");
    auto* values = pc->AsActorValueOwner();
    if (!values) return reject("no actor values");
    const auto* profile = Profile(equipped);
    const bool dualStaff = profile && profile->kind == StaffKind::DualCast && IsDualCastEligible(spell);
    const float cost = CalculateStaffMagickaCost(spell, pc, dualStaff);
    const RE::ActorValue resource = profile && profile->kind == StaffKind::Stamina
        ? RE::ActorValue::kStamina : RE::ActorValue::kMagicka;
    const float available = values->GetActorValue(resource);
    if (dualStaff) SKSE::log::info("DUAL {} PREFLIGHT cost={:.2f} normalCost={:.2f} available={:.2f}",
        left ? "L" : "R", cost, spell->CalculateMagickaCost(pc), available);
    if (!std::isfinite(cost) || cost < 0.0f || !std::isfinite(available)) {
        return reject("invalid resource calculation");
    }
    // Swift F&F: require enough Magicka for one full shot BEFORE allowing
    // the native hold animation to begin. This is a read-only gate, NOT an
    // escrow: charging costs exactly zero Magicka and cancellation refunds
    // exactly zero. Recalculate and check again at SpellFire and on EVERY
    // subsequent 0.5-second rapid pulse. Native Concentration SPELs retain
    // their existing no-full-cost-upfront policy.
    const bool isConcentration =
        spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
    const bool swiftFF = profile && profile->kind == StaffKind::Swift && !isConcentration;
    if ((!isConcentration && available + 0.001f < cost) ||
        (isConcentration && resource == RE::ActorValue::kStamina && cost > 0.0f && available <= 0.0f)) {
        const auto& config = GetConfig();
        if (config.showInsufficientMagickaMessage) {
            if (resource == RE::ActorValue::kStamina && !config.insufficientStaminaMessage.empty())
                RE::DebugNotification(config.insufficientStaminaMessage.c_str());
            else if (!config.insufficientMagickaMessage.empty())
                RE::DebugNotification(config.insufficientMagickaMessage.c_str());
        }
        return reject("insufficient casting resource");
    }
    if (swiftFF) {
        SKSE::log::info("RAPID {} PREFLIGHT spell={:08X} need={:.2f} have={:.2f} (no debit)",
                        left ? "L" : "R", hand.spell, cost, available);
    }
    // Target-location validation intentionally occurs AFTER native release:
    // this follows Spell Hotbar 2's CastSpellImmediate + state check, unlike
    // the unreliable pre-cast raycast introduced in v0.6.22.
    if (isConcentration || swiftFF) return true;
    const float duration = enchantment_->GetChargeTime();
    if (!std::isfinite(duration) || duration < 0.0f || duration > kMaxStaffChargeTime) {
        return reject("invalid charge duration");
    }
    auto& charge = charges_[idx];
    charge = {};
    charge.staff = hand.staff;
    charge.spell = hand.spell;
    charge.cost = cost;
    charge.resource = resource;
    // BoundWeapon unequips the staff while its native effect is created.
    // Debit Stamina progressively during native charging as usual, but keep
    // that paid escrow on successful fire (do not refund and charge it again).
    // Only the self-delivered Stamina BoundWeapon gets this settlement policy.
    if (resource == RE::ActorValue::kStamina &&
        spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf) {
        for (const auto* effect : spell->effects) {
            if (effect && effect->baseEffect &&
                effect->baseEffect->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kBoundWeapon) {
                charge.staminaBoundFrameEscrow = true;
                break;
            }
        }
    }
    charge.duration = duration;
    charge.active = true;
    SKSE::log::info("CHARGE {} BEGIN spell={:08X} cost={:.2f} duration={:.3f} staminaBoundFrameEscrow={}",
                    left ? "L" : "R", hand.spell, cost, duration, charge.staminaBoundFrameEscrow);
    return true;
}

void Controller::CancelCharge(bool left, std::string_view reason) {
    const auto idx = left ? 0u : 1u;
    const auto old = charges_[idx];
    if (!old.active) return;
    charges_[idx] = {}; // clear before any native calls/reentrancy
    firedThisPress_[idx] = true; // late SpellFire may not spend after a refund
    gateBlocked_[idx] = true;  // no new native attack until release/new press
    if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
        if (auto* values = pc->AsActorValueOwner(); values && std::isfinite(old.paid) && old.paid > 0.0f) {
            values->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                      old.resource, old.paid);
        }
    }
    SKSE::log::info("CHARGE {} REFUND {:.2f} reason={}", left ? "L" : "R", old.paid, reason);
}

void Controller::UpdateCharges(float dt) {
    if (!charges_[0].active && !charges_[1].active) return;
    auto* pc = RE::PlayerCharacter::GetSingleton();
    if (!pc) { charges_ = {}; return; }
    for (std::size_t idx = 0; idx < charges_.size(); ++idx) {
        auto& charge = charges_[idx];
        if (!charge.active) continue;
        const bool left = idx == 0;
        auto* equipped = pc->GetEquippedObject(left);
        if (!equipped || equipped->GetFormID() != charge.staff ||
            hands_[idx].staff != charge.staff || hands_[idx].spell != charge.spell ||
            pc->IsDead()) {
            CancelCharge(left, "equipment, binding or death");
            continue;
        }
        auto* native = pc->GetMagicCaster(left ? RE::MagicSystem::CastingSource::kLeftHand
                                               : RE::MagicSystem::CastingSource::kRightHand);
        const bool owned = native && native->currentSpell == enchantment_ &&
                           native->state.underlying() != 0;
        if (!owned) {
            if (charge.seenNative) {
                // Keep the ledger through the release frame. The synchronous
                // SpellFire callback commits it before stable idle cancels it.
                if (++charge.idleFrames >= 2) CancelCharge(left, "native cast ended without fire");
            } else {
                charge.waiting += dt;
                // No debit before native casting is observed. Vanilla can reject
                // an accepted input (stagger, furniture, sprint etc.).
                if (charge.waiting >= 1.0f) CancelCharge(left, "native cast never started");
            }
            continue;
        }
        charge.seenNative = true;
        charge.idleFrames = 0;
        // After releasing, stop advancing escrow while native release/interrupt
        // resolves. Final fire settles any unpaid remainder synchronously.
        if (!attackHeld_[idx]) continue;
        charge.elapsed = std::min(charge.duration, charge.elapsed + dt);
        const float fraction = charge.duration > 0.0f ? charge.elapsed / charge.duration : 1.0f;
        const float due = std::max(0.0f, charge.cost * fraction - charge.paid);
        auto* values = pc->AsActorValueOwner();
        if (!values) { CancelCharge(left, "actor values missing"); continue; }
        const float before = values->GetActorValue(charge.resource);
        const float remaining = std::max(0.0f, charge.cost - charge.paid);
        if (!std::isfinite(before) || before + 0.001f < remaining) {
            // External drain during charging: refund and suppress final fire.
            // Interrupt ONLY the exact shared staff ENCH in this hand; never kOther.
            const auto chargedStaff = charge.staff;
            CancelCharge(left, "resource lost during charge");
            using Interrupt = void (*)(RE::MagicCaster*, bool);
            static REL::Relocation<Interrupt> interrupt{REL::RelocationID(33630, 34408)};
            // Refund may invoke other native hooks. Reacquire the live caster
            // and verify equipment/ownership again before interruption.
            auto* livePlayer = RE::PlayerCharacter::GetSingleton();
            auto* liveCaster = livePlayer ? livePlayer->GetMagicCaster(left
                ? RE::MagicSystem::CastingSource::kLeftHand
                : RE::MagicSystem::CastingSource::kRightHand) : nullptr;
            auto* liveItem = livePlayer ? livePlayer->GetEquippedObject(left) : nullptr;
            if (liveCaster && liveItem && liveItem->GetFormID() == chargedStaff &&
                liveCaster->currentSpell == enchantment_ && liveCaster->state.underlying() != 0) {
                interrupt(liveCaster, false);
            }
            continue;
        }
        // Every F&F escrow, INCLUDING Stamina BoundWeapon, now debits
        // gradually on the same frame lifecycle. BoundWeapon differs only
        // in CastBoundSpell() settlement after a successful native SpellFire.
        if (due > 0.0f) {
            values->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                      charge.resource, -due);
            const float after = values->GetActorValue(charge.resource);
            // Never refund a debit the actor-value system did not actually apply.
            if (std::isfinite(after)) charge.paid += std::clamp(before - after, 0.0f, due);
        }
    }
}

void Controller::RefundLoadedCharge() {
    const float magicka = std::exchange(loadedChargeRefund_, 0.0f);
    const float stamina = std::exchange(loadedStaminaRefund_, 0.0f);
    auto* pc = RE::PlayerCharacter::GetSingleton();
    auto* values = pc ? pc->AsActorValueOwner() : nullptr;
    if (!values) return;
    for (const auto& [resource, amount] : {
        std::pair{RE::ActorValue::kMagicka, magicka},
        std::pair{RE::ActorValue::kStamina, stamina}
    }) {
        if (!std::isfinite(amount) || amount <= 0.0f) continue;
        values->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, resource, amount);
        SKSE::log::info("CHARGE co-save escrow refund resource={} amount={:.2f}",
            resource == RE::ActorValue::kStamina ? "Stamina" : "Magicka", amount);
    }
}
} // namespace SpellCastingStaff
