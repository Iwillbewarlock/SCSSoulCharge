#pragma once
// Stable SE 1.5.97 menu-hook / native-cast controller.
// All controller state remains in a single singleton instance; split into translation units.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include "SpellCastingStaff/SpellMemory.h"
#include <vector>
#include "SpellCastingStaff/StaffProfile.h"

namespace SpellCastingStaff {
    constexpr std::string_view kPluginName = "SpellCastingStaff.esp";
    constexpr std::uint32_t kSerializationID = 0x46534353;  // SCSF: renamed plugin co-save identity
    constexpr std::uint32_t kRecordHands = 0x444E4148;
    constexpr std::uint32_t kRecordVersion = 1;
    // One fixed-size SavedHand pair per remembered base WEAP form.
    constexpr std::uint32_t kRecordMemory = 0x4D454D53; // SMEM
    constexpr std::uint32_t kRecordMemoryVersion = 1;
    constexpr std::uint32_t kRecordEscrow = 0x574F5252; // preserve v0.6.16 co-save ID

    inline const char* SpellName(const RE::SpellItem* spell) {
        const char* name = spell ? spell->GetName() : nullptr;
        return name && name[0] ? name : "<unnamed spell>";
    }

    constexpr std::array<std::uint32_t, 6> kBuffIDs{0x0008C7, 0x000911, 0x000912, 0x000913, 0x000914, 0x000915};
    // SCS_Stamina01..05 -> BuffofStaffStamina01..05 (verified against supplied ESP).
    constexpr std::array<std::uint32_t, 5> kStaminaBuffIDs{0x000978, 0x000979, 0x00097A, 0x00097B, 0x00097C};
    // Index is tier - 2 for Swift/Focus, tier - 3 for DualCast.
    constexpr std::array<std::uint32_t, 4> kSwiftBuffIDs{0x00097D, 0x00097E, 0x00097F, 0x000980};
    constexpr std::array<std::uint32_t, 4> kFocusBuffIDs{0x000991, 0x000992, 0x000993, 0x000994};
    constexpr std::array<std::uint32_t, 3> kDualCastBuffIDs{0x00099B, 0x00099C, 0x00099D};
    // SCS_Eternity01..05 -> BuffofStaffEternity01..05, plugin-local FormIDs.
    constexpr std::array<std::uint32_t, 5> kEternityBuffIDs{0x0009BD, 0x0009AB, 0x0009AC, 0x0009AD, 0x0009AE};
    constexpr std::uint32_t kRecordEscrowVersion = 2;
    constexpr std::uint32_t kKeywordID = 0x0803;
    constexpr std::uint32_t kEnchantmentID = 0x080B;

    // Skyrim.esm only. SNDR IDs supplied by the user, no additional ESP records.
    constexpr std::uint32_t kFrostKeywordID = 0x01CEAE; // MagicDamageFrost
    constexpr std::uint32_t kShockKeywordID = 0x01CEAF; // MagicDamageShock
    constexpr std::array<std::uint32_t, 3> kFireSoundIDs{
        0x03C8FB, // MAGFirebolt03FireSD (default, including fire spells)
        0x03EAC6, // MAGIcicleFire2DSD (frost)
        0x03F206  // MAGShockFFFireSD (shock)
    };

    // Optional integration. Toxic Toss does NOT need to be installed.
    // Only the canonical plugin filename is supported.
    constexpr std::uint32_t kToxicTossSpellID = 0x12C8;
    constexpr std::string_view kToxicTossFile = "Toxic Toss.esp";

    struct SavedHand {
        RE::FormID staff{};
        RE::FormID spell{};
    };
    struct SavedHands {
        SavedHand left{};
        SavedHand right{};
    };
    static_assert(sizeof(SavedHands) == 16);

    // Runtime-only: never serialized. A channel may use kOther only while
    // no other plugin has claimed it. currentSpell MUST match before interrupt.
    struct ChannelState {
        RE::FormID staff{};
        RE::FormID spell{};
        RE::MagicSystem::CastingSource casterSource{RE::MagicSystem::CastingSource::kOther};
        bool running{};
        float costPerSecond{};
        RE::ActorValue resource{RE::ActorValue::kMagicka};
        // Arm the auxiliary native-hand watchdog only after observing our
        // dummy ENCH casting at least once. Other mods may use a different
        // hand-caster layout while our owned kOther channel remains valid.
        bool nativeSeen{};
        bool nativeDiagLogged{};
        float nativeIdleSeconds{};
    };

    // Runtime charge escrow. Only paid (actual AV debit) may be refunded.
    // Swift F&F repeats ONLY while the engine's actual staff-hand ENCH is
    // running. Runtime-only: no timer survives a load or an input release.
    struct RapidState {
        RE::FormID staff{};
        RE::FormID spell{};
        float elapsed{};
        bool running{};
    };

    struct ChargeState {
        RE::FormID staff{};
        RE::FormID spell{};
        float cost{};
        float paid{};
        RE::ActorValue resource{RE::ActorValue::kMagicka};
        float duration{};
        float elapsed{};
        float waiting{};
        unsigned idleFrames{};
        bool active{};
        bool seenNative{};
        // Stamina BoundWeapon: debit progressively during native charge.
        // Unlike other staffs, NEVER restore/re-debit this escrow on fire.
        bool staminaBoundFrameEscrow{};
    };

    // kOther is a shared caster. Track only a fire-and-forget cast this plugin
    // directly started; never infer ownership from an occupied caster alone.
    // Runtime-only: not written to the SKSE co-save.
    struct PendingFireState {
        RE::PlayerCharacter* player{};
        RE::MagicCaster* caster{};
        RE::SpellItem* spell{}; // pointer identity only; never dereference after load
        RE::FormID spellID{};
        std::chrono::steady_clock::time_point startedAt{};
        float activeSeconds{};
    };

    struct HandState {
        RE::FormID staff{};
        RE::FormID spell{};
    };

    class Controller final : public RE::BSTEventSink<RE::TESEquipEvent>,
                             public RE::BSTEventSink<RE::BSAnimationGraphEvent>,
                             public RE::BSTEventSink<RE::InputEvent*>,
                             public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static Controller& Get();
        bool ResolveForms();
        void Attach();
        void AttachInput();
        // Called exactly once per active game frame from the native main-loop hook.
        void OnFrame(float deltaSeconds);
        void PreviewInventoryStaffNames();
        // Called BEFORE vanilla AttackBlockHandler::ProcessButton.
        bool AllowStaffAttack(RE::ButtonEvent* button);
        void ObserveAttack(RE::ButtonEvent* button);
        // The input sink only publishes a snapshot; consumption is on OnFrame.
        void StageInputSinkAttack(std::size_t index, bool held);
        [[nodiscard]] bool LifecycleReady() const noexcept { return lifecycleReady_.load(std::memory_order_acquire); }
        void SetFrameHookReady(bool ready) { frameHookReady_ = ready; }
        void DisableForCompatibilityFailure();
        void CleanupDisabledStaffBuffsAfterLoad();
        [[nodiscard]] bool HasCompatibilityFailure() const noexcept {
            return compatibilityFailure_.load(std::memory_order_acquire);
        }
        // Called synchronously from the vanilla Magic Menu equip call site.
        // A true return means the selected spell was learned WITHOUT equipping it.
        bool TryInterceptMenuSpellEquip(RE::Actor* actor, RE::SpellItem* spell,
                                      const RE::BGSEquipSlot* targetSlot);
        void OnPostLoadGame();
        void OnNewGame();
        void OnRevert();
        // Only suppress Swift/DualCast compensating PERKs for Conc/Bound.
        [[nodiscard]] bool SuppressInversePerkForSpell(const RE::SpellItem* spell) const;
        // Effect-side narrow ownership check; never changes ESP perk or MGEF conditions.
        bool OwnsDualCastSpell(RE::TESObjectREFR* caster, RE::MagicItem* spell, RE::MagicSystem::CastingSource source) const;
        [[nodiscard]] bool OwnsDualCastEffect(const RE::ActiveEffect* effect) const;

        RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent*,
                                              RE::BSTEventSource<RE::TESEquipEvent>*) override;
        RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent*,
                                              RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override;
        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const*,
                                              RE::BSTEventSource<RE::InputEvent*>*) override;
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent*,
                                              RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

        void Save(SKSE::SerializationInterface* serial);
        void Load(SKSE::SerializationInterface* serial);

    private:
        // ESP GlobalVariable SCS_StaminaRateZero is a live casting-state gate,
        // independent of the scoped Variable08 effect/perk getter.
        void SyncStaminaCastingGlobal();
        void ResetStaminaCastingGlobal();
        void UpdateCharges(float dt);
        void CancelCharge(bool left, std::string_view reason);
        void RefundLoadedCharge();
        void QueueSync();
        void Sync();
        void ObserveAttackState(std::size_t index, bool held);
        void DrainStagedInput();
        bool IsStaff(const RE::TESForm* form) const;
        const StaffProfile* Profile(const RE::TESForm* form) const;
        int Tier(const RE::TESForm* form) const;
        int StaffIndex(const RE::TESForm* form) const;
        bool ValidSpell(RE::PlayerCharacter* player, RE::SpellItem* spell, const RE::TESForm* staff) const;
        void UpdateBuffs(RE::PlayerCharacter* player);
        void SyncNativeEnchantmentProperties(RE::PlayerCharacter* player);
        void RestoreNativeEnchantmentProperties();
        void Fire(bool left);                // receives the animation event
        bool CastBoundSpell(bool left);       // true only for a committed native fire
        void StopRapid(bool left, std::string_view reason);
        void UpdateRapid(float dt);          // 0.5s bursts, gated by native staff cast
        void UpdatePendingFire(float dt, std::string_view trigger); // owned kOther recovery
        // Dynamic MGEF SNDD lookup; original MGEFs untouched and no SH2 audio handles touched.
        // The verified shared dummy ENCH synchronizes only castingType and chargeTime.
        void PlayFireSound(RE::PlayerCharacter* player, RE::SpellItem* spell, bool left);
        void PlayConcentrationStartSound(RE::PlayerCharacter* player, RE::SpellItem* spell, bool left);
        void StopConcentrationSound(bool left);
        void StopChannel(bool left, std::string_view reason);
        void StartConcentration(bool left, RE::PlayerCharacter* player, RE::SpellItem* spell);
        void UpdateChannelWatch(float dt);
        [[nodiscard]] bool SpellHotbarClaimsOther() const;
        void ApplyTierName(RE::PlayerCharacter* player, std::size_t tier, const std::string& name);
        void RestoreAllNames();
        void RefreshNames(RE::PlayerCharacter* player);
        void RegisterPlayerAnimationSink();

        std::atomic<bool> compatibilityFailure_{false}; // fatal required menu hook; never cleared during this process
        bool enabled_ = false;
        bool gameReady_ = false; // no actor work between revert and post-load/new-game
        std::atomic<bool> lifecycleReady_{false}; // off-thread Bound Finish gate, never restores old-world slots
        std::uint64_t syncGeneration_ = 0; // discard tasks queued for an older world
        bool nativeEnchantmentSyncPending_ = false;
        bool originalNativeEnchantmentCaptured_ = false;
        RE::MagicSystem::CastingType originalNativeEnchantmentCastType_{RE::MagicSystem::CastingType::kConcentration};
        float originalNativeEnchantmentChargeTime_ = 1.0f;
        bool frameHookReady_ = false;
        bool nativeCastInProgress_ = false; // guard reentrant animation/native callbacks
        bool boundLeftDummyHandoff_ = false; // suppress only synchronous native dummy interrupt tags
        // The native AttackHook must continue working synchronously. The input
        // sink cannot safely mutate these fields from an unrelated callback
        // thread; it only publishes (native sequence, last held state).
        // OnFrame discards a staged event if a newer native attack ran.
        std::array<std::atomic<std::uint64_t>, 2> nativeAttackSequence_{};
        std::array<std::atomic<std::uint64_t>, 2> stagedInput_{};
        std::array<bool, 2> attackHeld_{};
        std::array<bool, 2> attackStateKnown_{};
        std::array<bool, 2> firedThisPress_{}; // latch reset only on new attack DOWN
        std::array<std::chrono::steady_clock::time_point, 2> attackPressedAt_{};
        std::chrono::steady_clock::time_point lastMissingSpellNotice_{};
        bool equipSinkAdded_ = false;
        bool inputSinkAdded_ = false;
        bool menuSinkAdded_ = false;
        // Retry animation-event subscription after a new game or load only for a
        // bounded startup window. Later graph rebuilds are handled by an on-demand
        // check just before the first staff attack.
        float animationSinkCheckCountdown_ = 0.0f;
        float animationSinkStartupRetryRemaining_ = 0.0f;
        bool animationSinkStartupDeferred_ = false;
        RE::FormID preferredStaff_ = 0;
        std::array<ChannelState, 2> channels_{};
        std::array<RapidState, 2> rapid_{};
        PendingFireState pendingFire_{};
        std::array<ChargeState, 2> charges_{};
        std::array<bool, 2> gateHeld_{};
        std::array<bool, 2> gateBlocked_{};
        std::array<bool, 2> drawOnlyPress_{};
        float loadedChargeRefund_{}; // legacy v1 magicka escrow
        float loadedStaminaRefund_{}; // v2 stamina escrow
        // Cross-thread equip callbacks set only the atomic request flag.
        // Only the native frame callback drains it and mutates equipment,
        // preventing the observed input-sink ↔ task Sync() overlap.
        std::atomic<bool> syncQueued_{false};
        std::atomic<bool> inventoryNamePreviewPending_{false};
        int buffTier_ = -1;
        RE::SpellItem* appliedBuff_ = nullptr;
        std::vector<RE::TESObjectWEAP*> staffs_;
        std::vector<std::string> originalNames_;
        std::vector<std::string> appliedNames_;
        std::array<RE::SpellItem*, 6> buffs_{};
        std::array<RE::SpellItem*, 5> staminaBuffs_{};
        std::array<RE::SpellItem*, 4> swiftBuffs_{};
        std::array<RE::SpellItem*, 4> focusBuffs_{};
        std::array<RE::SpellItem*, 3> dualCastBuffs_{};
        std::array<RE::SpellItem*, 5> eternityBuffs_{};
        std::array<RE::BGSKeyword*, kStaffProfiles.size()> profileKeywords_{};
        RE::BGSKeyword* keyword_ = nullptr;
        RE::EnchantmentItem* enchantment_ = nullptr;
        std::array<RE::BGSSoundDescriptorForm*, 3> fireSounds_{};
        std::array<RE::BSSoundHandle, 2> concentrationLoopHandles_{}; // OUR sounds only
        RE::SpellItem* toxicThrowSpell_ = nullptr;
        // Optional Spell Hotbar 2 read-only globals from SpellHotbar.esp.
        // NEVER write them: Spell Hotbar 2 owns its own casting lifecycle.
        RE::TESGlobal* spellHotbarCastingSource_ = nullptr;
        RE::TESGlobal* staminaCastingGlobal_ = nullptr; // SpellCastingStaff.esp local 0x9B7; owned by us
        RE::BGSEquipSlot* slotLeft_ = nullptr;
        RE::BGSEquipSlot* slotRight_ = nullptr;
        std::array<HandState, 2> hands_{};
        SpellMemory rememberedSpells_;
        std::atomic<bool> validateLoadedPending_{false};
    };
} // namespace SpellCastingStaff
