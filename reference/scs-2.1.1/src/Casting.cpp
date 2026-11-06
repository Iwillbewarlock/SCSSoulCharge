// Native immediate fire and engine-side left Bound Weapon restore fix (introduced in v0.6.34).
//
// Original SmartStaff Papyrus J3XStaffEffectScript.CastSpell():
//   Spell.Cast(caster, None); caster.DamageAV("Magicka", cost);
// This implementation retains the *animation event* as its trigger and uses
// the native MagicCaster, never a Papyrus call. Do NOT commandeer the actual
// left/right hand caster while occupied. v0.6.27 uses the verified-idle
// actual left caster for self-delivered left-hand BoundWeapon spells.
//
// v0.6.11+: concentration uses kOther ONLY after checking that the
// caster is idle and Spell Hotbar 2 does not claim it. Native lifecycle is
// sampled from the existing frame callback; no task-recursive loops.
#include "SpellCastingStaff/Controller.h"
#include "SpellCastingStaff/ProjectileDualFlag.h"
#include "SpellCastingStaff/Config.h"
#include "SpellCastingStaff/DualCastCost.h"
#include "SpellCastingStaff/CasterDiagnostics.h"
#include <RE/A/ActorMagicCaster.h>
#include <RE/E/Effect.h>
#include <RE/E/EffectSetting.h>
#include <spdlog/spdlog.h>
#include "SpellCastingStaff/ReviewPolicy.h"
#include "SpellCastingStaff/PerkEntryProbe.h"

namespace SpellCastingStaff {

namespace {
    // Avoid interrupting ordinary native cleanup while a rune/projectile
    // is being created. These are fail-safe delays, NOT proof of cast success.
    constexpr float kOrphanedFireGraceSeconds = 0.5f;
    // On some load-order / frame-hook combinations the frame callback may
    // not advance our timer. Allow next attack to recover after wall-clock
    // delay without inventing a new per-frame hook or task queue.
    constexpr auto kFallbackFireGrace = std::chrono::milliseconds(500);
    // RE::MagicCaster::State::kCasting is not exported under that enumerator
    // name in the user's installed CommonLibSSE-NG 3.6.0 headers. The native
    // underlying state was observed to be 6 in both stuck-rune game logs.
    // Compare the stored numeric state, not a header-specific enum label.
    constexpr std::uint32_t kNativeCastingState = 6u;
}

// IMPORTANT (SE 1.5.97 / CommonLibSSE-NG 3.6.0):
// MagicCaster::InterruptCast(bool) is the engine's NON-VIRTUAL shutdown
// procedure at Address Library ID 33630 (AE:34408).  Its native code handles
// the actual caster state cleanup.  Calling only InterruptCastImpl(false)
// (a virtual internal hook) DOES NOT substitute for this public engine function:
// v0.6.11's earlier fallback left the kOther caster stuck at spell/state=6.
//
// Call the same native routine wrapped by CommonLibSSE-NG MagicCaster.cpp.
// Keeping this small wrapper avoids depending on different header exports of
// InterruptCast across installed 3.6.0 variants.
// Never call Actor::InterruptCast(): that can stop unrelated hand casters.
void InterruptOwnedCaster(RE::MagicCaster* caster) {
    if (!caster) { return; }
    using interrupt_t = void (*)(RE::MagicCaster*, bool);
    static REL::Relocation<interrupt_t> nativeInterrupt{REL::RelocationID(33630, 34408)};
    nativeInterrupt(caster, false);
}

// Run only on the main thread: called from the existing frame lifecycle and
// immediately before a new fire. Identity + spell + state must all match the
// invocation we recorded; never interrupt the left/right staff casters.
void Controller::UpdatePendingFire(float dt, std::string_view trigger) {
    if (!pendingFire_.spellID || !gameReady_ || nativeCastInProgress_) { return; }
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* caster = player ? player->GetMagicCaster(RE::MagicSystem::CastingSource::kOther) : nullptr;
    if (!player || player->IsDead() || player != pendingFire_.player ||
        !caster || caster != pendingFire_.caster) {
        pendingFire_ = {};
        return;
    }
    const auto state = caster->state.underlying();
    if (!caster->currentSpell && state == 0) {
        SKSE::log::info("FIRE kOther released naturally spell={:08X}", pendingFire_.spellID);
        pendingFire_ = {};
        return;
    }
    // Another spell, different casting state, or third-party reservation:
    // the original ownership cannot safely be proven anymore.
    if (SpellHotbarClaimsOther() || channels_[0].running || channels_[1].running ||
        caster->currentSpell != pendingFire_.spell ||
        state != kNativeCastingState) {
        SKSE::log::warn("FIRE kOther watch abandoned expected={:08X} actual={:08X} state={} hotbar={}",
                        pendingFire_.spellID,
                        caster->currentSpell ? caster->currentSpell->GetFormID() : 0u,
                        state, SpellHotbarClaimsOther());
        pendingFire_ = {};
        return;
    }
    if (std::isfinite(dt) && dt > 0.0f) {
        // Clamp pathological frame delta; a single hitches shouldn't force
        // interrupt of a cast during the same render frame.
        pendingFire_.activeSeconds += std::min(dt, 0.25f);
    }
    const bool enoughGameTime = pendingFire_.activeSeconds >= kOrphanedFireGraceSeconds;
    const bool enoughWallTime =
        trigger == "next attack" &&
        std::chrono::steady_clock::now() - pendingFire_.startedAt >= kFallbackFireGrace;
    if (!enoughGameTime && !enoughWallTime) { return; }

    const auto spellID = pendingFire_.spellID;
    pendingFire_ = {}; // one-shot, disarm before calling native code
    InterruptOwnedCaster(caster);
    const auto endID = caster->currentSpell ? caster->currentSpell->GetFormID() : 0u;
    const auto endState = caster->state.underlying();
    SKSE::log::warn("FIRE kOther orphan recovery trigger={} spell={:08X} result={:08X}/{}",
                    trigger, spellID, endID, endState);
    if (endID != 0u || endState != 0u) {
        SKSE::log::error("FIRE kOther native interrupt did not return caster to idle; no raw state writes");
    }
}

void Controller::Fire(bool left) {
    if (!enabled_ || !gameReady_ || nativeCastInProgress_) { return; }
    const auto idx = left ? 0u : 1u;
    auto* pc = RE::PlayerCharacter::GetSingleton();
    if (!pc || pc->IsDead()) { return; }
    auto* equipped = pc->GetEquippedObject(left);
    const auto& hand = hands_[idx];
    if (!equipped || Tier(equipped) < 0 ||
        !hand.spell || hand.staff != equipped->GetFormID()) { return; }

    const auto now = std::chrono::steady_clock::now();
    // A leftover SpellFire tag from the PREVIOUS staff attack can arrive a few
    // milliseconds after a fresh attack DOWN. The one-fire-per-press latch by
    // itself cannot identify such a stale tag, because the new DOWN rearms it.
    // In the 2026-10-19 test log, normal staff charging took ~1.3 seconds,
    // while the erroneous second Fire was only 34 ms after its new DOWN.
    // Reject implausibly early tags without consuming this press's fire token.
    // This is a conservative diagnostic window, not a proven animation ID.
    // Bound the historical 200 ms safeguard by the current native charge time.
    // Instant casts have no early-tag window. This remains a heuristic, not
    // proof of animation identity; the per-press latch remains authoritative.
    const float duration = enchantment_ ? enchantment_->GetChargeTime() : 0.0f;
    const auto kMinPressToFire = std::chrono::duration<float>(
        std::isfinite(duration) ? std::clamp(duration * 0.5f, 0.0f, 0.2f) : 0.2f);
    if (attackStateKnown_[idx] && attackHeld_[idx] &&
        attackPressedAt_[idx] != std::chrono::steady_clock::time_point{} &&
        now - attackPressedAt_[idx] < kMinPressToFire) {
        SKSE::log::warn("FIRE {} ignored: animation tag {} ms after latest DOWN (possible stale tag)",
                        left ? "L" : "R",
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - attackPressedAt_[idx]).count());
        return;
    }
    // Swift: the borrowed native Concentration ENCH must still be actively
    // casting when SpellFire arrives. A late tag after key-up is never a shot.
    // Ordinary F&F intentionally continues to accept its normal release tag.
    const auto* fireProfile = Profile(equipped);
    auto* fireSpell = RE::TESForm::LookupByID<RE::SpellItem>(hand.spell);
    const bool swiftFF = fireProfile && fireProfile->kind == StaffKind::Swift && fireSpell &&
        fireSpell->GetCastingType() == RE::MagicSystem::CastingType::kFireAndForget;
    if (swiftFF) {
        auto* native = pc->GetMagicCaster(left ? RE::MagicSystem::CastingSource::kLeftHand
                                               : RE::MagicSystem::CastingSource::kRightHand);
        if (!attackStateKnown_[idx] || !attackHeld_[idx] ||
            !native || native->currentSpell != enchantment_ ||
            native->state.underlying() == 0) {
            SKSE::log::info("RAPID {} ignored SpellFire: native staff cast not active",
                            left ? "L" : "R");
            return;
        }
    }
    // Skyrim animations sometimes emit several SpellFire tags for ONE held
    // attack. A wall-clock debounce of 60ms did not stop a second cast when
    // tags were ~1s apart. Use the player's actual attack press as the ID.
    // ATTACK_UP intentionally does not rearm: a trailing animation tag after
    // release still belongs to the same press. Next ATTACK_DOWN rearms.
    // All supported casts require observed attack input: F&F needs an approved
    // charge, concentration needs a held button. No timer-only fallback.
    if (!attackStateKnown_[idx] || firedThisPress_[idx]) { return; }
    firedThisPress_[idx] = true;
    // v0.6.30: fire immediately on the native staff SpellFire event.
    // The v0.6.28 one-frame deferral demonstrably did not correct the
    // bound equipment teardown and has been removed completely.
    const bool committed = CastBoundSpell(left);
    // Every rejected final cast returns its escrow; a successful one consumed it.
    CancelCharge(left, "fire finished without committing charge");
    if (!committed) {
        if (swiftFF) {
            // An approved native staff hold cannot continue auto-firing if
            // its first full-cost shot was rejected. Interrupt only our ENCH.
            auto* native = pc->GetMagicCaster(left ? RE::MagicSystem::CastingSource::kLeftHand
                                                   : RE::MagicSystem::CastingSource::kRightHand);
            if (native && native->currentSpell == enchantment_ && native->state.underlying() != 0)
                InterruptOwnedCaster(native);
        }
        return;
    }
    // Original Concentration spells retain their existing channel path.
    // Swift repeat is reserved for ordinary Fire-and-Forget spells; repeated
    // BoundWeapon equip/teardown can corrupt hand restoration.
    auto* actual = RE::TESForm::LookupByID<RE::SpellItem>(hand.spell);
    const auto* profile = Profile(equipped);
    if (!profile || profile->kind != StaffKind::Swift || !actual ||
        actual->GetCastingType() != RE::MagicSystem::CastingType::kFireAndForget ||
        !attackHeld_[idx]) return;
    if (actual->GetDelivery() == RE::MagicSystem::Delivery::kSelf) {
        for (const auto* effect : actual->effects) {
            if (effect && effect->baseEffect &&
                effect->baseEffect->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kBoundWeapon)
                return;
        }
    }
    auto* native = pc->GetMagicCaster(left ? RE::MagicSystem::CastingSource::kLeftHand
                                          : RE::MagicSystem::CastingSource::kRightHand);
    if (!native || native->currentSpell != enchantment_ ||
        native->state.underlying() == 0) return;
    rapid_[idx] = {hand.staff, hand.spell, 0.0f, true};
    SKSE::log::info("RAPID {} START staff={:08X} spell={:08X} interval=0.50s",
                    left ? "L" : "R", hand.staff, hand.spell);
}

bool Controller::CastBoundSpell(bool left) {
    const char* side = left ? "L" : "R";
    auto* pc = RE::PlayerCharacter::GetSingleton();
    if (!pc || pc->IsDead()) { return false; }
    // Never fire while two SmartStaff items are simultaneously shown as
    // equipped. The older equipment resolution logic handles that case.
    if (IsStaff(pc->GetEquippedObject(true)) &&
        IsStaff(pc->GetEquippedObject(false))) {
        SKSE::log::warn("FIRE {} blocked: two SpellCastingStaff items equipped", side);
        return false;
    }
    const auto idx = left ? 0u : 1u;
    auto* weapon = pc->GetEquippedObject(left);
    const auto& hand = hands_[idx];
    if (!weapon || Tier(weapon) < 0 ||
        !hand.spell || hand.staff != weapon->GetFormID()) {
        SKSE::log::info("FIRE {} cancelled: staff binding changed", side);
        return false;
    }
    auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(hand.spell);
    if (!ValidSpell(pc, spell, weapon)) {
        SKSE::log::warn("FIRE {} blocked: invalid or unlearned spell {:08X}", side, hand.spell);
        return false;
    }
    // Spell power is supplied exclusively by the ESP PERK via VirtualAV08.
    // Never pass a staff-specific effectiveness multiplier to native casting.
    // Concentration is isolated from the fully verified fire-and-forget path.
    if (spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
        StartConcentration(left, pc, spell);
        return false;
    }
    if (spell->GetCastingType() != RE::MagicSystem::CastingType::kFireAndForget) {
        SKSE::log::warn("FIRE {} blocked: unsupported casting type", side);
        return false;
    }
    if (nativeCastInProgress_) {
        SKSE::log::warn("FIRE {} dropped: reentrant native casting", side);
        return false;
    }
    auto* values = pc->AsActorValueOwner();
    if (!values) { return false; }
    const auto* profile = Profile(weapon);
    // Swift F&F has no charge ledger. Each native CastSpellImmediate call is
    // a single full-price transaction, starting only at the SpellFire tag.
    // Legacy Basic/Stamina/Focus/etc. keep the proven escrow mechanism.
    const bool swiftDirect = profile && profile->kind == StaffKind::Swift;
    const auto& charge = charges_[idx];
    if (!swiftDirect && (!charge.active || charge.staff != hand.staff || charge.spell != hand.spell)) {
        SKSE::log::info("FIRE {} blocked: no approved charge", side);
        return false;
    }
    if (swiftDirect && (charge.active || !attackStateKnown_[idx] || !attackHeld_[idx])) {
        SKSE::log::warn("RAPID {} blocked: unexpected escrow or attack released", side);
        return false;
    }
    const float cost = swiftDirect ? spell->CalculateMagickaCost(pc) : charge.cost;
    float prepaid = swiftDirect ? 0.0f : charge.paid;
    float boundSettlementDebit = 0.0f;
    const auto resource = swiftDirect ? RE::ActorValue::kMagicka : charge.resource;
    const bool staminaBoundFrameEscrow = !swiftDirect && charge.staminaBoundFrameEscrow;
    if (staminaBoundFrameEscrow && resource != RE::ActorValue::kStamina) {
        SKSE::log::error("FIRE {} rejected inconsistent Stamina BoundWeapon ledger", side);
        return false;
    }
    const bool staminaStaff = resource == RE::ActorValue::kStamina;
    float available = values->GetActorValue(resource);
    if (!std::isfinite(cost) || cost < 0.0f || !std::isfinite(prepaid) ||
        prepaid < 0.0f || prepaid > cost + 0.001f || !std::isfinite(available)) {
        SKSE::log::warn("FIRE {} blocked: invalid casting resource (cost={}, have={})", side, cost, available);
        return false;
    }
    if (available + prepaid + 0.001f < cost) {
        SKSE::log::info("FIRE {} blocked: insufficient casting resource for '{}' (cost={:.2f}, have={:.2f})",
                        side, SpellName(spell), cost, available);
        return false;
    }

    // Papyrus Spell.Cast(caster, None) did not hand the victim/current combat
    // target to the engine. Aimed/target-location spells must use the engine's
    // normal targeting logic instead of forcing the current combat target.
    // Only kSelf explicitly supplies the player as target.
    auto* target = spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf
        ? static_cast<RE::TESObjectREFR*>(pc) : nullptr;
    auto source = RE::MagicSystem::CastingSource::kOther;
    // Spell Hotbar 2 may be charging a spell on kOther but not yet own
    // currentSpell. Do not race that pending cast.
    if (SpellHotbarClaimsOther()) {
        SKSE::log::info("FIRE {} blocked: Spell Hotbar 2 has reserved kOther", side);
        return false;
    }
    auto* caster = pc->GetMagicCaster(source);
    if (!caster) {
        SKSE::log::warn("FIRE {} blocked: no independent MagicCaster", side);
        return false;
    }
    // Fallback for frame callbacks that have not advanced our watchdog:
    // this can reclaim ONLY the exact kOther cast we started at least 0.5s
    // ago. Other mods' occupied casters remain untouched.
    UpdatePendingFire(0.0f, "next attack");
    // Never interrupt an unrelated mod's spell, and never write to the hand
    // caster occupied by SpellCastingStaff's native ENCH.
    if (caster->currentSpell || caster->state.underlying() != 0) {
        SKSE::log::warn("FIRE {} blocked: independent caster occupied spell={:08X} state={}",
                        side, caster->currentSpell ? caster->currentSpell->GetFormID() : 0u,
                        caster->state.underlying());
        return false;
    }
    // This particular kOther caster is idle, unreserved and ours to use.
    // Never inherit a stale dual flag from a previous F&F staff shot. The
    // first staff cast after a cold start previously logged false -> true,
    // but a later cast observed previous=true even though the earlier scope
    // restored false. Do NOT transfer that state to a normal concentration.
    if (caster->GetIsDualCasting()) {
        caster->SetDualCasting(false);
        SKSE::log::warn("FIRE {} stale idle kOther dual cleared spell={:08X} remaining={}",
                        side, spell->GetFormID(), caster->GetIsDualCasting());
    }
    // Only self-delivered BoundWeapon effects need a native hand source.
    // Actor::GetMagicCaster returns native ActorMagicCaster instances.
    // Right-hand bound spells retain the tested kOther source override.
    bool boundHandSource = false;
    if (spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf) {
        for (const auto* effect : spell->effects) {
            if (effect && effect->baseEffect && effect->baseEffect->GetArchetype() ==
                RE::EffectArchetypes::ArchetypeID::kBoundWeapon) {
                boundHandSource = true;
                break;
            }
        }
    }
    if (boundHandSource && caster->GetCastingSource() != source) {
        SKSE::log::warn("BOUND source override blocked: kOther reports source={}",
                        static_cast<int>(caster->GetCastingSource()));
        return false; // Fire() refunds the still-armed charge.
    }
    // The diagnostic game log proves the actual left caster is idle at this
    // SpellFire callback. A source-only override on kOther records source=0,
    // yet left bound teardown restores the staff to the RIGHT hand. Test the
    // actual left caster identity as well, without borrowing a busy caster.
    const bool useActualLeftCaster = boundHandSource && left;
    if (useActualLeftCaster) {
        auto* handCaster = pc->GetMagicCaster(RE::MagicSystem::CastingSource::kLeftHand);
        const bool ownsApprovedStaffCharge = !swiftDirect && charge.active &&
            charge.staff == hand.staff && charge.spell == hand.spell && charge.seenNative;
        if (handCaster && (swiftDirect || ownsApprovedStaffCharge) &&
            handCaster->currentSpell == enchantment_ && handCaster->state.underlying() != 0 &&
            handCaster->GetCastingSource() == RE::MagicSystem::CastingSource::kLeftHand &&
            attackStateKnown_[idx] && (swiftDirect ? attackHeld_[idx] : true)) {
            // All staff profiles may have their own native dummy ENCH occupying
            // the actual left hand at SpellFire. Keep the verified escrow and
            // its cancellation/refund ledger intact through this one interrupt.
            // Never interrupt an unrelated left-hand spell or use kOther as a
            // fallback (which restores a BoundWeapon into the wrong hand).
            struct HandoffScope {
                bool& flag;
                explicit HandoffScope(bool& f) : flag(f) { flag = true; }
                ~HandoffScope() { flag = false; }
            } handoff{boundLeftDummyHandoff_};
            InterruptOwnedCaster(handCaster);
            SKSE::log::info("BOUND LEFT dummy stopped kind={} spell={:08X} state={} escrow={:.2f}",
                static_cast<int>(profile->kind),
                handCaster->currentSpell ? handCaster->currentSpell->GetFormID() : 0u,
                handCaster->state.underlying(), prepaid);
        }
        if (!handCaster || handCaster->currentSpell || handCaster->state.underlying() != 0 ||
            handCaster->GetCastingSource() != RE::MagicSystem::CastingSource::kLeftHand) {
            SKSE::log::warn("BOUND LEFT blocked: actual hand caster not idle/left; no override, interrupt, or fallback");
            return false; // No ownership -> no interruption/fallback.
        }
        caster = handCaster;
        source = RE::MagicSystem::CastingSource::kLeftHand;
    }
    nativeCastInProgress_ = true;
    struct CastScope {
        bool& active;
        ~CastScope() { active = false; }
    } guard{nativeCastInProgress_};

    // DualCast staves use the engine's real ActorMagicCaster dual-cast flag.
    // Escrow cost has already been calculated with the game's dual-cost GMSTs.
    // Our kOther is idle/owned above; hand BoundWeapon is also idle/verified.
    // No persistent player high-process changes here: dedicated effect-side hooks
    // temporarily provide the Actor::IsDualCasting view only during native callbacks.
    // Restore the original flag even on early returns (rune failure, invalid AV).
    const bool staffDualCast = profile && profile->kind == StaffKind::DualCast && IsDualCastEligible(spell);
    const bool previousDualCast = staffDualCast ? caster->GetIsDualCasting() : false;
    if (staffDualCast) {
        caster->SetDualCasting(true);
        SKSE::log::info("DUAL {} FIRE set={} previous={} spell={:08X}", side,
            caster->GetIsDualCasting(), previousDualCast, spell->GetFormID());
    }
    struct ScopedDualCastRestore {
        RE::MagicCaster* caster;
        bool enabled;
        bool previous;
        ~ScopedDualCastRestore() { if (enabled && caster) caster->SetDualCasting(previous); }
    } dualRestore{caster, staffDualCast, previousDualCast};

    SKSE::log::info("FIRE {} BEGIN spell='{}' id={:08X} source={} delivery={} cost={:.2f} resource={:.2f}",
                    side, SpellName(spell), spell->GetFormID(), static_cast<int>(source),
                    static_cast<int>(spell->GetDelivery()), cost, available);
    // Retain bound Stamina escrow, then collect ONLY the unpaid balance.
    // Commit before native casting can unequip the staff. On failure the live
    // ledger still records the actual debit so Fire()->CancelCharge refunds it.
    if (staminaBoundFrameEscrow) {
        const float framePaid = prepaid;
        const auto settled = CollectEscrowBalance(cost, prepaid,
            [&] { return values->GetActorValue(resource); },
            [&](float due) { values->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, resource, -due); });
        charges_[idx].paid = prepaid;
        boundSettlementDebit = prepaid - framePaid;
        if (!settled.success) {
            SKSE::log::error("BOUND SETTLE failed spell={:08X} need={} paid={} after={}",
                spell->GetFormID(), cost, prepaid, settled.available);
            return false;
        }
        available = settled.available;
    }

    // Disarm before entering native code; only ordinary escrow is restored.
    if (!swiftDirect) charges_[idx] = {};
    if (prepaid > 0.0f && !staminaBoundFrameEscrow) {
        values->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                  resource, prepaid);
    }
    const float beforeEngine = values->GetActorValue(resource);
    const float beforeEngineMagicka = staminaStaff
        ? values->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
    if (!std::isfinite(beforeEngine)) {
        SKSE::log::error("FIRE {} aborted: invalid resource after escrow restoration", side);
        return false;
    }
    // RestoreActorValue can be capped by the actor's maximum, e.g. a potion
    // filled magicka while holding a charged staff. Only the ACTUAL restore
    // undoes prepaid cost; the un-restored portion remains already paid.
    const float restored = std::clamp(beforeEngine - available, 0.0f, prepaid);
    const float retainedPrepaid = prepaid - restored;
    if (useActualLeftCaster) {
        SKSE::log::info("BOUND LEFT actual hand caster BEGIN spell={:08X} source={}",
            spell->GetFormID(), static_cast<int>(caster->GetCastingSource()));
        {
            CasterDiagnostics::ScopedStaffBoundCast perkScope(pc, spell, source);
            caster->CastSpellImmediate(spell, false, target, 1.0f, false, 0.0f,
                                       target ? nullptr : pc);
        }
        SKSE::log::info("BOUND LEFT actual hand caster END spell={:08X} state={} source={}",
            caster->currentSpell ? caster->currentSpell->GetFormID() : 0u,
            caster->state.underlying(), static_cast<int>(caster->GetCastingSource()));
    } else if (boundHandSource) {
        auto* actorCaster = static_cast<RE::ActorMagicCaster*>(caster);
        const auto savedSource = actorCaster->castingSource;
        const auto handSource = left ? RE::MagicSystem::CastingSource::kLeftHand
                                     : RE::MagicSystem::CastingSource::kRightHand;
        {
            // Scope ends immediately after the synchronous native call. Do not
            // leave a hand source installed for sound, watchdog or later frames.
            struct SourceScope {
                RE::ActorMagicCaster* caster;
                RE::MagicSystem::CastingSource saved;
                ~SourceScope() { caster->castingSource = saved; }
            } sourceGuard{actorCaster, savedSource};
            actorCaster->castingSource = handSource;
            SKSE::log::info("BOUND source BEGIN spell={:08X} hand={} saved={} effective={}",
                            spell->GetFormID(), side, static_cast<int>(savedSource),
                            static_cast<int>(caster->GetCastingSource()));
            CasterDiagnostics::ScopedStaffBoundCast perkScope(pc, spell, handSource);
            caster->CastSpellImmediate(spell, false, target, 1.0f, false, 0.0f,
                                       target ? nullptr : pc);
        }
        SKSE::log::info("BOUND source RESTORED spell={:08X} source={} expected={}",
                        spell->GetFormID(), static_cast<int>(caster->GetCastingSource()),
                        static_cast<int>(savedSource));
    } else {
        // Native cast. The ESP Mod Spell Magnitude perk performs all scaling;
        // no temporary SPEL/MGEF changes and no magnitudeOverride experiment.
        CasterDiagnostics::ScopedStaffBoundCast perkScope(pc, spell, source);
        caster->CastSpellImmediate(spell, false, target, 1.0f, false, 0.0f,
                                   target ? nullptr : pc);
    }
    // Stamina staffs pay via our escrow, NOT via native Magicka.
    // Some native caster paths still debit Magicka synchronously despite a
    // staff's chosen resource. Undo ONLY the measured, bounded native cost.
    // A remaining asynchronous/native concentration drain is diagnosed separately.
    if (staminaStaff && std::isfinite(beforeEngineMagicka)) {
        const float magAfter = values->GetActorValue(RE::ActorValue::kMagicka);
        if (std::isfinite(magAfter)) {
            const float engineMagickaDebit = std::clamp(beforeEngineMagicka - magAfter,
                0.0f, cost);
            if (engineMagickaDebit > 0.0f) {
                values->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                    RE::ActorValue::kMagicka, engineMagickaDebit);
                SKSE::log::warn("STAMINA FIRE native Magicka debit refunded={:.2f} spell={:08X}",
                    engineMagickaDebit, spell->GetFormID());
            }
        }
    }
    // Spell Hotbar 2 (casting_controller.cpp) found a reliable failure signal
    // for fire-and-forget TARGET LOCATION spells: after CastSpellImmediate,
    // remaining in the exact same kOther spell/state=6 means that the rune or
    // summon placement was rejected.  It immediately interrupts such casts
    // and does not commit any mana cost.  This is NOT an extra raycast: Skyrim
    // still decides valid range, location and collision itself.
    //
    // Our staff pre-pays some/all cost while charging.  Unlike Hotbar, we must
    // explicitly undo that escrow along with any immediate native debit, while
    // avoiding a second refund when Fire() calls CancelCharge afterwards.
    // The ledger above was disarmed, so this is the only rollback path.
    const bool targetLocationFailed =
        spell->GetDelivery() == RE::MagicSystem::Delivery::kTargetLocation &&
        caster->currentSpell == spell &&
        caster->state.underlying() == kNativeCastingState;
    if (targetLocationFailed) {
        // A failed cast still played its animation.  Preserve the expected
        // release sound (as in Spell Hotbar 2) without a false success log.
        PlayFireSound(pc, spell, left);
        const auto afterNative = values->GetActorValue(resource);
        const float nativeDebit = std::isfinite(afterNative)
            ? std::max(0.0f, beforeEngine - afterNative) : 0.0f;
        // Native interruption must never touch hand ENCH or another mod's
        // kOther cast. We just invoked this matching spell synchronously.
        // If Hotbar unexpectedly took ownership, leave the caster untouched.
        const bool mayInterrupt = !SpellHotbarClaimsOther() &&
            caster->currentSpell == spell &&
            caster->state.underlying() == kNativeCastingState;
        if (mayInterrupt) {
            InterruptOwnedCaster(caster);
        }
        pendingFire_ = {};
        // We originally paid prepaid, restored exactly 'restored', and native
        // may have paid nativeDebit. Return only the remainder + native debit.
        // ActorValue::RestoreActorValue clamps to the actual maximum, so a
        // concurrent potion/regen cannot make this exceed maximum magicka.
        const float refund = retainedPrepaid + nativeDebit;
        if (std::isfinite(refund) && refund > 0.0f) {
            values->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                      resource, refund);
        }
        SKSE::log::warn("FIRE {} TARGET_LOCATION_FAILED spell={:08X} rollback={:.2f} (prepaidRetained={:.2f} nativeDebit={:.2f}) interrupted={} final={:08X}/{}",
                        side, spell->GetFormID(), refund, retainedPrepaid,
                        nativeDebit, mayInterrupt,
                        caster->currentSpell ? caster->currentSpell->GetFormID() : 0u,
                        caster->state.underlying());
        return false;
    }

    // Unexpected kOther occupancy (including non-target-location spells) is
    // still covered by the 0.5s ownership-checked failsafe watchdog.
    if (useActualLeftCaster) {
        // Hand casters have a native lifecycle. Never put one in the kOther
        // watchdog, force idle, or clear an unrelated pending kOther record.
        if (caster->currentSpell || caster->state.underlying() != 0) {
            SKSE::log::warn("BOUND LEFT hand caster remains active after immediate call; native lifecycle retained");
        }
    } else if (caster->currentSpell == spell &&
        caster->state.underlying() == kNativeCastingState) {
        pendingFire_ = {pc, caster, spell, spell->GetFormID(),
                        std::chrono::steady_clock::now(), 0.0f};
        SKSE::log::info("FIRE {} watchdog armed spell={:08X} state=6", side, spell->GetFormID());
    } else {
        pendingFire_ = {};
        SKSE::log::info("FIRE {} native result spell={:08X} state={} (watchdog not needed)",
                        side, caster->currentSpell ? caster->currentSpell->GetFormID() : 0u,
                        caster->state.underlying());
    }
    // Sound is emitted only on the validated native-cast path. Rejected
    // SpellFire tags, invalid spells, concentration and insufficient magicka
    // do NOT reach this point. The cast API is void; actual hit is not known.
    PlayFireSound(pc, spell, left);

    // Original Papyrus Spell.Cast charged the cost explicitly. Some native
    // caster paths may also charge, so measure the immediate engine debit and
    // charge only the uncovered remainder. Verify deferred cost separately.
    const float postEngine = values->GetActorValue(resource);
    if (!std::isfinite(postEngine)) {
        SKSE::log::error("FIRE {} ended with invalid native resource; no further AV writes", side);
        return false;
    }
    const float enginePaid = std::max(0.0f, beforeEngine - postEngine);
    // If native casting also charged a full cost, undo only the overlap with
    // our retained advance. Never refund more than our own retained payment.
    const float overlap = std::min(retainedPrepaid,
        std::max(0.0f, retainedPrepaid + enginePaid - cost));
    if (overlap > 0.0f) {
        values->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                  resource, overlap);
    }
    // BoundWeapon Stamina was settled before entering the native cast.
    // Do not debit again after the synchronous cast.
    const float remainder = staminaBoundFrameEscrow ? 0.0f :
        std::max(0.0f, cost - retainedPrepaid - enginePaid);
    if (remainder > 0.0f) {
        values->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                  resource, -remainder);
    }
#if !defined(SCS_UNIT_TEST)
    // Tag ONLY the physical projectile created by a successful DualCast staff
    // aimed F&F cast. The native effect/actor bits are not altered here.
    if (staffDualCast && source == RE::MagicSystem::CastingSource::kOther &&
        spell->GetCastingType() == RE::MagicSystem::CastingType::kFireAndForget &&
        spell->GetDelivery() == RE::MagicSystem::Delivery::kAimed) {
        ProjectileDualFlag::Arm(pc, spell);
    }
#endif
    SKSE::log::info("FIRE {} END spell='{}' cost={:.2f} payment={} (advance={:.2f}, restored={:.2f}, retained={:.2f}, engine={:.2f}, overlapRefund={:.2f}, manual={:.2f})",
                    side, SpellName(spell), cost, swiftDirect ? "rapid/direct" : "charge/escrow",
                    prepaid, restored, retainedPrepaid, enginePaid, overlap, remainder);
    if (staminaBoundFrameEscrow) {
        const float finalStamina = values->GetActorValue(RE::ActorValue::kStamina);
        SKSE::log::info("STAMINA BOUND FRAME-ESCROW spell={:08X} prepaid={:.2f} cost={:.2f} atFire={:.2f} after={:.2f} extraNativeRefund={:.2f} additionalDebit={:.2f}",
            spell->GetFormID(), prepaid, cost, available, finalStamina, overlap, boundSettlementDebit + remainder);
    }
    return true;
}

// Swift's timer does not own the native hand caster. The hand's own
// concentration animation is the authority; no input-only or stale timer fire.
void Controller::StopRapid(bool left, std::string_view reason) {
    auto& rapid = rapid_[left ? 0u : 1u];
    if (!rapid.running) return;
    SKSE::log::info("RAPID {} STOP spell={:08X} reason={}",
                    left ? "L" : "R", rapid.spell, reason);
    rapid = {};
}

void Controller::UpdateRapid(float dt) {
    if (!rapid_[0].running && !rapid_[1].running) return;
    auto* pc = RE::PlayerCharacter::GetSingleton();
    if (!pc || pc->IsDead()) {
        StopRapid(true, "player missing or dead");
        StopRapid(false, "player missing or dead");
        return;
    }
    for (std::size_t idx = 0; idx < rapid_.size(); ++idx) {
        auto& rapid = rapid_[idx];
        if (!rapid.running) continue;
        const bool left = idx == 0;
        const auto* staff = pc->GetEquippedObject(left);
        const auto* profile = Profile(staff);
        auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(rapid.spell);
        auto* native = pc->GetMagicCaster(left ? RE::MagicSystem::CastingSource::kLeftHand
                                               : RE::MagicSystem::CastingSource::kRightHand);
        // This gate precedes EVERY repeat, including the frame the interval
        // expires. Never synthesize a shot after native animation has ended.
        if (!attackStateKnown_[idx] || !attackHeld_[idx] ||
            !pc->AsActorState() || !pc->AsActorState()->IsWeaponDrawn() ||
            !staff || staff->GetFormID() != rapid.staff ||
            hands_[idx].staff != rapid.staff || hands_[idx].spell != rapid.spell ||
            !profile || profile->kind != StaffKind::Swift ||
            !spell || spell->GetCastingType() != RE::MagicSystem::CastingType::kFireAndForget ||
            !ValidSpell(pc, spell, staff) ||
            !native || native->currentSpell != enchantment_ ||
            native->state.underlying() == 0 || SpellHotbarClaimsOther()) {
            StopRapid(left, "native animation, input, binding, or ownership lost");
            continue;
        }
        // Never backfill missed shots after a large frame hitch. No rapid
        // bursts if the game resumes from a pause/stall.
        if (dt > 0.25f) {
            StopRapid(left, "large frame gap");
            continue;
        }
        rapid.elapsed += dt;
        if (rapid.elapsed < 0.5f) continue;
        rapid.elapsed = std::fmod(rapid.elapsed, 0.5f);
        auto* av = pc->AsActorValueOwner();
        if (!av || charges_[idx].active || channels_[idx].running) {
            StopRapid(left, "invalid ledger or competing channel");
            continue;
        }
        // Each pulse is an INSTANT, full-cost native cast. No ChargeState is
        // allocated, and no charge/refund accounting runs during native hold.
        const float cost = spell->CalculateMagickaCost(pc);
        const float available = av->GetActorValue(RE::ActorValue::kMagicka);
        if (!std::isfinite(cost) || cost < 0.0f || !std::isfinite(available) ||
            available + 0.001f < cost) {
            StopRapid(left, "insufficient magicka or invalid cost");
            // Only interrupt the EXACT native staff ENCH we verified. Never
            // interrupt another hand caster or kOther occupied by SH2.
            if (native->currentSpell == enchantment_ && native->state.underlying() != 0)
                InterruptOwnedCaster(native);
            continue;
        }
        // A preflight native hand check is required again on every pulse;
        // the call is synchronous on Skyrim's main/game thread.
        if (!CastBoundSpell(left)) {
            StopRapid(left, "rapid native fire failed");
            // No escrow exists for Swift: do not initiate refund or retry.
            if (native->currentSpell == enchantment_ && native->state.underlying() != 0)
                InterruptOwnedCaster(native);
            continue;
        }
    }
}

// Spell Hotbar 2 sets SpellHotbar_CastingSource to the caster reserved for
// the CURRENT hotbar animation and resets it to 0 when done. kOther=2.
// Read-only, optional; when SH2 is absent the native caster gate still works.
bool Controller::SpellHotbarClaimsOther() const {
    if (!spellHotbarCastingSource_) { return false; }
    const float value = spellHotbarCastingSource_->value;
    if (!std::isfinite(value)) { return true; } // corrupted shared state: fail closed
    // Stay in floating point: huge finite values must not overflow lround/int.
    return std::round(value) == static_cast<float>(RE::MagicSystem::CastingSource::kOther);
}

void Controller::StartConcentration(bool left, RE::PlayerCharacter* player, RE::SpellItem* spell) {
    const auto idx = left ? 0u : 1u;
    const char* side = left ? "L" : "R";
    if (!enabled_ || !gameReady_ || !player || !spell || !frameHookReady_ || !inputSinkAdded_) {
        SKSE::log::info("CONC {} blocked: unavailable frame/input lifecycle", side);
        return;
    }
    if (!attackStateKnown_[idx] || !attackHeld_[idx] ||
        channels_[0].running || channels_[1].running || nativeCastInProgress_) {
        SKSE::log::info("CONC {} blocked: button not held, already channeling, or reentrant", side);
        return;
    }
    if (SpellHotbarClaimsOther()) {
        SKSE::log::warn("CONC {} blocked: Spell Hotbar 2 reserved kOther", side);
        return;
    }
    UpdatePendingFire(0.0f, "next attack");
    auto* values = player->AsActorValueOwner();
    if (!values) return;
    const auto* profile = Profile(player->GetEquippedObject(left));
    // ALL concentration uses native single-cast cost. A DualCast staff's
    // normal dual cost is reserved exclusively for eligible F&F spells.
    const float cost = spell->CalculateMagickaCost(player);
    const bool staminaStaff = profile && profile->kind == StaffKind::Stamina;
    constexpr auto source = RE::MagicSystem::CastingSource::kOther;
    auto* caster = player->GetMagicCaster(source);
    if (!caster || caster->currentSpell || caster->state.underlying() != 0) {
        SKSE::log::warn("CONC {} blocked: independent caster occupied", side);
        return;
    }
    // Concentration is explicitly single-cast, even on a DualCast staff.
    // The only safe time to clear stale F&F dual state is while our kOther is
    // confirmed idle and Spell Hotbar 2 has not reserved it.
    if (caster->GetIsDualCasting()) {
        caster->SetDualCasting(false);
        SKSE::log::warn("CONC {} stale idle kOther dual cleared spell={:08X} remaining={}",
                        side, spell->GetFormID(), caster->GetIsDualCasting());
    }
    const auto resource = staminaStaff ? RE::ActorValue::kStamina : RE::ActorValue::kMagicka;
    const float available = values->GetActorValue(resource);
    // Cost is a per-second rate, not a full cost required upfront.
    if (!std::isfinite(cost) || cost < 0.0f || !std::isfinite(available) ||
        (staminaStaff && cost > 0.0f && available <= 0.0f)) {
        SKSE::log::info("CONC {} blocked: cost={} resource={}", side, cost, available);
        return;
    }
    const auto* equipped = player->GetEquippedObject(left);
    if (!equipped || Tier(equipped) < 0 ||
        equipped->GetFormID() != hands_[idx].staff ||
        spell->GetFormID() != hands_[idx].spell || !ValidSpell(player, spell, equipped)) return;

    auto* target = spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf
        ? static_cast<RE::TESObjectREFR*>(player) : nullptr;
    nativeCastInProgress_ = true;
    struct CastScope { bool& active; ~CastScope() { active = false; } } guard{nativeCastInProgress_};

    // Stamina staffs are separately accounted per frame; for Magicka leave
    // the ongoing drain to the engine. No dual flags are ever set here.
    caster->currentSpellCost = staminaStaff ? 0.0f : cost;
    const float magickaBeforeNative = staminaStaff
        ? values->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
    {
        CasterDiagnostics::ScopedStaffBoundCast perkScope(player, spell, source);
        caster->CastSpellImmediate(spell, false, target, 1.0f, false, 0.0f,
                                   target ? nullptr : player);
    }
    const bool owned = caster->currentSpell == spell && caster->state.underlying() != 0;
    if (staminaStaff) {
        const float afterMana = values->GetActorValue(RE::ActorValue::kMagicka);
        const float nativeCost = std::isfinite(magickaBeforeNative) && std::isfinite(afterMana)
            ? std::max(0.0f, magickaBeforeNative - afterMana) : 0.0f;
        if (!std::isfinite(caster->currentSpellCost) ||
            std::abs(caster->currentSpellCost) > 0.0001f || nativeCost > 0.001f) {
            SKSE::log::error("STAMINA CONC rejected: nativeCostSetting={} immediateMagickaDebit={:.3f}",
                caster->currentSpellCost, nativeCost);
            if (owned && !SpellHotbarClaimsOther()) InterruptOwnedCaster(caster);
            if (!caster->currentSpell && caster->state.underlying() == 0 && !SpellHotbarClaimsOther())
                caster->currentSpellCost = 0.0f;
            return;
        }
    }
    if (!owned) {
        if (!caster->currentSpell && caster->state.underlying() == 0)
            caster->currentSpellCost = 0.0f;
        SKSE::log::warn("CONC {} native start rejected", side);
        return;
    }
    auto& channel = channels_[idx];
    channel = {};
    channel.running = true;
    channel.staff = hands_[idx].staff;
    channel.spell = spell->GetFormID();
    channel.casterSource = source;
    channel.costPerSecond = cost;
    channel.resource = resource;
    PlayConcentrationStartSound(player, spell, left);
    SKSE::log::info("CONC {} START staff={:08X} spell={:08X} source=kOther cost/s={:.2f}",
                    side, channel.staff, channel.spell, cost);
}

// Stop only the recorded caster and exact spell. If SH2 has reserved kOther,
// abandon our bookkeeping without touching the shared caster, even when the
// current spell ID happens to match (both mods may cast the same spell).
void Controller::StopChannel(bool left, std::string_view reason) {
    const auto idx = left ? 0u : 1u;
    const auto saved = channels_[idx];
    if (!saved.running) { return; }
    channels_[idx] = {}; // disarm BEFORE possible animation reentrancy
    // Clean up only OUR loop handle on *every* exit, including ownership lost.
    // Concentration STOP intentionally has no extra MGEF release cue.
    StopConcentrationSound(left);
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) { return; }
    if (saved.casterSource == RE::MagicSystem::CastingSource::kOther &&
        SpellHotbarClaimsOther()) {
        SKSE::log::warn("CONC {} relinquish: Spell Hotbar 2 has kOther (reason={})",
                        left ? "L" : "R", reason);
        return;
    }
    auto* caster = player->GetMagicCaster(saved.casterSource);
    auto* expected = RE::TESForm::LookupByID<RE::SpellItem>(saved.spell);
    if (caster && !caster->currentSpell && caster->state.underlying() == 0) {
        SKSE::log::info("CONC {} already finished natively (reason={})", left ? "L" : "R", reason);
        return; // ordinary depletion is not an ownership error; no interrupt needed
    }
    // The original v0.6.7 candidate also interrupted on currentSpell=null.
    // DO NOT: a non-idle/empty caster doesn't prove ownership.
    if (!caster || !expected || caster->currentSpell != expected ||
        caster->state.underlying() == 0) {
        SKSE::log::warn("CONC {} relinquish: ownership no longer proven (reason={})",
                        left ? "L" : "R", reason);
        return;
    }
    const auto beforeSpell = caster->currentSpell ? caster->currentSpell->GetFormID() : 0u;
    const auto beforeState = caster->state.underlying();
    InterruptOwnedCaster(caster);
    const auto afterSpell = caster->currentSpell ? caster->currentSpell->GetFormID() : 0u;
    const auto afterState = caster->state.underlying();
    SKSE::log::info("CONC {} STOP_NATIVE_VERIFY spell={:08X}/{} -> {:08X}/{} reason={}",
                    left ? "L" : "R", beforeSpell, beforeState, afterSpell, afterState, reason);
    if (afterSpell != 0u || afterState != 0u) {
        SKSE::log::warn("CONC {} native interrupt returned while kOther was still occupied; do not manually clear engine caster state",
                        left ? "L" : "R");
    }
    // Reset cost only when kOther is safely idle after our interruption.
    if (!caster->currentSpell && caster->state.underlying() == 0 &&
        !SpellHotbarClaimsOther()) {
        caster->currentSpellCost = 0.0f;
    }
    SKSE::log::info("CONC {} STOP reason={}", left ? "L" : "R", reason);
}

void Controller::UpdateChannelWatch(float dt) {
    if (!channels_[0].running && !channels_[1].running) { return; }
    auto* pc = RE::PlayerCharacter::GetSingleton();
    if (!pc) {
        StopConcentrationSound(true);
        StopConcentrationSound(false);
        channels_ = {};
        return;
    }
    for (std::size_t idx = 0; idx < channels_.size(); ++idx) {
        auto& channel = channels_[idx];
        if (!channel.running) { continue; }
        const bool left = idx == 0;
        if (pc->IsDead()) {
            StopChannel(left, "player dead");
            continue;
        }
        if (channel.casterSource == RE::MagicSystem::CastingSource::kOther &&
            SpellHotbarClaimsOther()) {
            StopChannel(left, "Spell Hotbar 2 reserved kOther");
            continue;
        }
        auto* caster = pc->GetMagicCaster(channel.casterSource);
        auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(channel.spell);
        if (!caster || !spell || caster->currentSpell != spell ||
            caster->state.underlying() == 0) {
            StopChannel(left, "caster finished or ownership lost");
            continue;
        }
        auto* form = pc->GetEquippedObject(left);
        if (!form || form->GetFormID() != channel.staff ||
            hands_[idx].staff != channel.staff || hands_[idx].spell != channel.spell) {
            StopChannel(left, "staff or binding changed");
            continue;
        }
        if (!attackStateKnown_[idx] || !attackHeld_[idx]) {
            StopChannel(left, "attack released or input state unknown");
            continue;
        }
        // A native staff hand may use another spell/state under other
        // animation or input mods, even while our kOther channel is valid.
        // Do not arm the native-hand loss guard until this channel has
        // actually observed its dummy ENCH in the native hand. Once armed,
        // tolerate 0.15s of absence to survive transient native transitions.
        // Held-input, animation stop, equipment/binding, resource and kOther
        // ownership guards remain independent of this diagnostic observation.
        auto* native = pc->GetMagicCaster(left ? RE::MagicSystem::CastingSource::kLeftHand
                                               : RE::MagicSystem::CastingSource::kRightHand);
        const auto* expectedHandSpell = static_cast<RE::MagicItem*>(enchantment_);
        const bool handCasting = native && native->currentSpell == expectedHandSpell &&
                                 native->state.underlying() != 0;
        if (handCasting) {
            channel.nativeSeen = true;
            channel.nativeIdleSeconds = 0.0f;
        } else {
            channel.nativeIdleSeconds += dt;
            if (!channel.nativeDiagLogged && channel.nativeIdleSeconds >= 0.1f) {
                channel.nativeDiagLogged = true;
                SKSE::log::warn("CONC {} native hand not casting: handSpell={:08X} handState={} expected={:08X} seen={}",
                    left ? "L" : "R",
                    native && native->currentSpell ? native->currentSpell->GetFormID() : 0u,
                    native ? static_cast<int>(native->state.underlying()) : -1,
                    enchantment_ ? enchantment_->GetFormID() : 0u, channel.nativeSeen);
            }
            if (channel.nativeSeen && channel.nativeIdleSeconds > 0.15f) {
                StopChannel(left, "native staff hand stopped");
                continue;
            }
        }
        auto* av = pc->AsActorValueOwner();
        if (!av) { StopChannel(left, "actor values missing"); continue; }
        const float available = av->GetActorValue(channel.resource);
        if (!std::isfinite(available)) {
            StopChannel(left, "invalid casting resource");
            continue;
        }
        if (channel.resource == RE::ActorValue::kStamina) {
            // If Skyrim recomputed native cost despite our zero, fail closed;
            // do not overwrite an occupied caster or forge free Magicka.
            if (!std::isfinite(caster->currentSpellCost) ||
                std::abs(caster->currentSpellCost) > 0.0001f) {
                SKSE::log::error("STAMINA CONC native cost nonzero: {} (unsupported engine drain)",
                    caster->currentSpellCost);
                StopChannel(left, "native cost unexpectedly active");
                continue;
            }
            if (channel.costPerSecond > 0.0f) {
                // A large game-frame gap should never perform an unbounded debit.
                // Prefer stopping the channel to silently charging late.
                if (dt > 0.25f) {
                    StopChannel(left, "large frame gap");
                    continue;
                }
                const float due = channel.costPerSecond * dt;
                if (!std::isfinite(due) || due < 0.0f || available + 0.001f < due) {
                    StopChannel(left, "stamina depleted");
                    continue;
                }
                av->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                    RE::ActorValue::kStamina, -due);
                const float after = av->GetActorValue(RE::ActorValue::kStamina);
                if (!std::isfinite(after) || (after <= 0.0f && channel.costPerSecond > 0.0f)) {
                    StopChannel(left, "stamina depleted after channel tick");
                    continue;
                }
            }
        } else if (channel.costPerSecond > 0.0f && available <= 0.0f) {
            StopChannel(left, "magicka depleted");
            continue;
        }
        // Native engine still owns the real Magicka channel drain; only
        // Stamina channels use the manual per-frame cost above.

    }
}

void Controller::ResetStaminaCastingGlobal() {
    if (staminaCastingGlobal_ && staminaCastingGlobal_->value != 0.0f) {
        staminaCastingGlobal_->value = 0.0f;
        SKSE::log::info("STAMINA REGEN GLOB reset=0");
    }
}

void Controller::SyncStaminaCastingGlobal() {
    if (!staminaCastingGlobal_) return;
    bool staffCasting = false;
    auto* pc = (enabled_ && gameReady_) ? RE::PlayerCharacter::GetSingleton() : nullptr;
    if (pc && !pc->IsDead()) {
        for (std::size_t idx = 0; idx < 2; ++idx) {
            const bool left = idx == 0;
            auto* equipped = pc->GetEquippedObject(left);
            const auto* profile = Profile(equipped);
            if (!profile || profile->kind != StaffKind::Stamina || !equipped ||
                hands_[idx].staff != equipped->GetFormID() || !hands_[idx].spell) continue;
            const auto& channel = channels_[idx];
            const bool ownChannel = channel.running && attackHeld_[idx] &&
                channel.resource == RE::ActorValue::kStamina &&
                channel.staff == hands_[idx].staff && channel.spell == hands_[idx].spell;
            auto* native = pc->GetMagicCaster(left
                ? RE::MagicSystem::CastingSource::kLeftHand
                : RE::MagicSystem::CastingSource::kRightHand);
            // Disallow stale native state alone from blocking regeneration when
            // the player is actually casting ONLY with the opposite hand.
            const auto& charge = charges_[idx];
            const bool ownReleasedCharge = charge.active && charge.seenNative &&
                charge.staff == hands_[idx].staff && charge.spell == hands_[idx].spell;
            const bool ownNativeCharge = native && native->currentSpell == enchantment_ &&
                native->state.underlying() != 0 &&
                (attackHeld_[idx] || ownReleasedCharge) &&
                native->GetCastingSource() == (left
                    ? RE::MagicSystem::CastingSource::kLeftHand
                    : RE::MagicSystem::CastingSource::kRightHand);
            if (ownChannel || ownNativeCharge) { staffCasting = true; break; }
        }
    }
    const float desired = staffCasting ? 1.0f : 0.0f;
    if (!std::isfinite(staminaCastingGlobal_->value) || staminaCastingGlobal_->value != desired) {
        staminaCastingGlobal_->value = desired;
        SKSE::log::info("STAMINA REGEN GLOB {} (owned native ENCH/channel only)", staffCasting ? "ON" : "OFF");
    }
}

void Controller::OnFrame(float dt) {
    if (!enabled_ || !gameReady_ || nativeCastInProgress_ || !frameHookReady_ || !std::isfinite(dt) || dt <= 0.0f) { return; }
    // Startup-only retry for delayed player graph creation. This does NOT run
    // forever: after the bounded window expires, later recovery relies on the
    // first staff-use check in AllowStaffAttack().
    if (animationSinkStartupRetryRemaining_ > 0.0f) {
        animationSinkStartupRetryRemaining_ = std::max(0.0f, animationSinkStartupRetryRemaining_ - dt);
        animationSinkCheckCountdown_ -= dt;
        if (animationSinkCheckCountdown_ <= 0.0f) {
            animationSinkCheckCountdown_ = 1.0f;
            RegisterPlayerAnimationSink();
            if (!animationSinkStartupDeferred_) {
                animationSinkStartupRetryRemaining_ = 0.0f;
                animationSinkCheckCountdown_ = 0.0f;
                SKSE::log::info("ANIM SINK startup retry completed");
            } else if (animationSinkStartupRetryRemaining_ <= 0.0f) {
                animationSinkCheckCountdown_ = 0.0f;
                SKSE::log::warn("ANIM SINK startup retry window expired; will retry on first staff use");
            }
        } else if (animationSinkStartupRetryRemaining_ <= 0.0f) {
            animationSinkCheckCountdown_ = 0.0f;
            SKSE::log::warn("ANIM SINK startup retry window expired; will retry on first staff use");
        }
    }
    // All input-sink fallback updates and queued equipment Sync() run on this
    // one existing native frame callback. Never call Sync from SKSE worker tasks.
    DrainStagedInput();
    if (syncQueued_.exchange(false, std::memory_order_acq_rel)) {
        if (enabled_ && lifecycleReady_.load(std::memory_order_acquire)) {
            Sync(); // Sync() itself has the paired-overlap instrumentation.
        }
    }
    // One pre-existing shared frame hook: no recursive task queue / timer.
    UpdateCharges(dt);
    UpdateChannelWatch(dt);
    UpdatePendingFire(dt, "frame");
    UpdateRapid(dt);
    SyncStaminaCastingGlobal(); // after ledger/channel cleanup, on the original native frame hook
#if !defined(SCS_UNIT_TEST)
    ProjectileDualFlag::Update();
#endif
    if (nativeEnchantmentSyncPending_) {
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            SyncNativeEnchantmentProperties(player);
        }
    }
}


} // namespace SpellCastingStaff
