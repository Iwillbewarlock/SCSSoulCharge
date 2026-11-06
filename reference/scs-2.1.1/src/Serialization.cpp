// v0.4.7 split from v0.4.6 plugin.cpp, without intentional logic changes.
#include "SpellCastingStaff/Controller.h"
#include <spdlog/spdlog.h>

namespace SpellCastingStaff {

    void Controller::Save(SKSE::SerializationInterface* serial) {
        if (!serial) {
            return;
        }
        // v1 co-saves store only Magicka (float). v2 keeps separate escrows
        // because unfinished Stamina charges must NEVER refund Magicka.
        struct EscrowV2 { float magicka; float stamina; };
        EscrowV2 escrow{};
        for (const auto& charge : charges_) {
            if (!charge.active || !std::isfinite(charge.paid) || charge.paid <= 0.0f) continue;
            if (charge.resource == RE::ActorValue::kStamina) escrow.stamina += charge.paid;
            else escrow.magicka += charge.paid;
        }
        if (!std::isfinite(escrow.magicka)) escrow.magicka = 0.0f;
        if (!std::isfinite(escrow.stamina)) escrow.stamina = 0.0f;
        if (!serial->WriteRecord(kRecordEscrow, kRecordEscrowVersion, escrow)) {
            SKSE::log::error("Failed to write resource escrow refund data");
        }
        const SavedHands record{{hands_[0].staff, hands_[0].spell}, {hands_[1].staff, hands_[1].spell}};
        if (!serial->WriteRecord(kRecordHands, kRecordVersion, record)) {
            SKSE::log::error("Failed to write SpellCastingStaff save data");
        }
        auto memories = rememberedSpells_.Snapshot();
        std::sort(memories.begin(), memories.end()); // deterministic record order
        for (const auto& [staff, spell] : memories) {
            const SavedHand memory{staff, spell};
            if (!serial->WriteRecord(kRecordMemory, kRecordMemoryVersion, memory)) {
                SKSE::log::error("Failed to write remembered staff spell");
            }
        }
    }

    void Controller::Load(SKSE::SerializationInterface* serial) {
        OnRevert();
        if (!serial) {
            return;
        }
        validateLoadedPending_.store(true, std::memory_order_release);
        std::uint32_t type = 0;
        std::uint32_t version = 0;
        std::uint32_t length = 0;
        // Hands, escrow and fixed-size remembered-spell records. Bound both
        // zero-length record floods and time spent draining corrupt payloads.
        constexpr std::uint32_t kMaxRecords = 1024;
        constexpr std::uint32_t kMaxPayloadBytes = 1024 * 1024;
        std::uint32_t recordCount = 0;
        std::uint32_t payloadBytes = 0;
        while (serial->GetNextRecordInfo(type, version, length)) {
            if (++recordCount > kMaxRecords || length > kMaxPayloadBytes - payloadBytes) {
                SKSE::log::error("Co-save record budget exceeded; remaining plugin data ignored");
                break; // still migrate any HAND record already read
            }
            payloadBytes += length;
            if (type == kRecordHands && version == kRecordVersion && length == sizeof(SavedHands)) {
                SavedHands record{};
                if (serial->ReadRecordData(record) == sizeof(record)) {
                    const SavedHand data[2] = {record.left, record.right};
                    for (std::size_t i = 0; i < 2; ++i) {
                        RE::FormID staffID = 0;
                        RE::FormID spellID = 0;
                        if (data[i].staff) {
                            if (!serial->ResolveFormID(data[i].staff, staffID)) staffID = 0;
                        }
                        if (data[i].spell) {
                            if (!serial->ResolveFormID(data[i].spell, spellID)) spellID = 0;
                        }
                        // During co-save load the form table should be available;
                        // if not, fail closed and do not bind an arbitrary spell.
                        if (Tier(RE::TESForm::LookupByID(staffID)) >= 0) {
                            hands_[i].staff = staffID;
                            hands_[i].spell = spellID;
                        }
                    }
                }
            } else if (type == kRecordMemory && version == kRecordMemoryVersion && length == sizeof(SavedHand)) {
                SavedHand memory{};
                if (serial->ReadRecordData(memory) != sizeof(memory)) {
                    SKSE::log::error("Truncated remembered staff spell record");
                    break;
                }
                RE::FormID staffID = 0;
                RE::FormID spellID = 0;
                if (memory.staff && memory.spell &&
                    serial->ResolveFormID(memory.staff, staffID) &&
                    serial->ResolveFormID(memory.spell, spellID) &&
                    StaffIndex(RE::TESForm::LookupByID(staffID)) >= 0 &&
                    RE::TESForm::LookupByID<RE::SpellItem>(spellID)) {
                    // Only our known base staffs: the map cannot grow beyond
                    // the staff registry even with duplicate/corrupt records.
                    rememberedSpells_.Set(staffID, spellID);
                }
            } else if (type == kRecordEscrow && version == 1 && length == sizeof(float)) {
                // Backward compatibility with v1.1.9 co-saves.
                float escrow = 0.0f;
                if (serial->ReadRecordData(escrow) == sizeof(escrow) &&
                    std::isfinite(escrow) && escrow >= 0.0f)
                    loadedChargeRefund_ = escrow;
            } else if (type == kRecordEscrow && version == kRecordEscrowVersion && length == 2 * sizeof(float)) {
                std::array<float, 2> escrow{};
                if (serial->ReadRecordData(escrow.data(), sizeof(escrow)) == sizeof(escrow) &&
                    std::isfinite(escrow[0]) && std::isfinite(escrow[1]) &&
                    escrow[0] >= 0.0f && escrow[1] >= 0.0f) {
                    loadedChargeRefund_ = escrow[0];
                    loadedStaminaRefund_ = escrow[1];
                }
            } else {
                // Skip unknown versions and other record types safely.
                // Record length comes from disk. Never allocate length bytes
                // for an unknown/corrupt record (could request gigabytes).
                std::array<std::uint8_t, 512> scratch{};
                auto remaining = length;
                bool truncated = false;
                while (remaining > 0) {
                    const auto requested = std::min<std::uint32_t>(remaining, 512u);
                    const auto read = serial->ReadRecordData(scratch.data(), requested);
                    if (!read || read > requested) {
                        SKSE::log::error("Truncated or invalid unknown co-save record");
                        truncated = true;
                        break;
                    }
                    remaining -= read;
                }
                if (truncated) break;
            }
        }
        // Migrate older saves without SMEM. Existing explicit memory wins,
        // independent of record order; learned-spell validation waits for Sync.
        for (const auto& hand : hands_) {
            if (StaffIndex(RE::TESForm::LookupByID(hand.staff)) >= 0 &&
                RE::TESForm::LookupByID<RE::SpellItem>(hand.spell)) {
                rememberedSpells_.InsertIfMissing(hand.staff, hand.spell);
            }
        }
    }

} // namespace SpellCastingStaff
