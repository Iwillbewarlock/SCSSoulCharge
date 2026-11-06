#include "PCH.h"

#include "serialization.h"

#include "conversion.h"
#include "pool.h"
#include "selftest.h"
#include "watch.h"

namespace scssc::serialization
{
	namespace
	{
		constexpr std::uint32_t kUniqueID = 'SCSC';  // SCS itself uses 'SCSF'
		constexpr std::uint32_t kPoolRecord = 'POOL';
		constexpr std::uint32_t kPoolRecordVersion = 1;
		// Written while conversion is blocked, so a later load stays blocked instead of starting a fresh table.
		constexpr std::uint32_t kBlockedRecord = 'BLCK';
		constexpr std::uint32_t kBlockedRecordVersion = 1;
		constexpr std::uint32_t kMaxRecordBytes = 8 + pool::kPoolSize * 8;

		enum class LoadState
		{
			kNone,      // no POOL record: save made without this mod (or a new game)
			kLoaded,
			kRejected,  // POOL present but unreadable
			kBlocked,   // the save was made while blocked
		};

		LoadState                 g_loadState = LoadState::kNone;
		std::vector<std::uint8_t> g_rawPool;  // the POOL bytes as loaded, re-written unchanged while blocked

		void OnSave(SKSE::SerializationInterface* a_intfc)
		{
			if (conversion::IsBlocked()) {
				const std::uint8_t flag = 1;
				a_intfc->WriteRecord(kBlockedRecord, kBlockedRecordVersion, flag);
				if (!g_rawPool.empty()) {
					a_intfc->WriteRecord(kPoolRecord, kPoolRecordVersion, g_rawPool.data(), static_cast<std::uint32_t>(g_rawPool.size()));
				}
				logger::warn("Co-save: saved while blocked (BLCK + original POOL {} bytes)", g_rawPool.size());
				return;
			}
			const auto bytes = pool::Table().Serialize();
			if (!a_intfc->WriteRecord(kPoolRecord, kPoolRecordVersion, bytes.data(), static_cast<std::uint32_t>(bytes.size()))) {
				logger::error("Co-save: writing POOL ({} bytes) failed", bytes.size());
				return;
			}
			logger::info("Co-save: POOL saved ({} slots)", pool::Table().Used());
		}

		void OnRevert(SKSE::SerializationInterface*)
		{
			conversion::SetSettled(false);
			watch::Reset();
			pool::Clear();
			g_loadState = LoadState::kNone;
			g_rawPool.clear();
		}

		void OnLoad(SKSE::SerializationInterface* a_intfc)
		{
			std::uint32_t type = 0;
			std::uint32_t version = 0;
			std::uint32_t length = 0;
			bool          blocked = false;
			while (a_intfc->GetNextRecordInfo(type, version, length)) {
				if (type == kBlockedRecord) {
					blocked = true;
					continue;
				}
				if (type != kPoolRecord) {
					logger::warn("Co-save: unknown record {:08X} skipped", type);
					continue;
				}
				if (version != kPoolRecordVersion || length > kMaxRecordBytes) {
					logger::error("Co-save: POOL version {} length {} not supported", version, length);
					g_loadState = LoadState::kRejected;
					continue;
				}
				g_rawPool.assign(length, 0);
				if (a_intfc->ReadRecordData(g_rawPool.data(), length) != length) {
					logger::error("Co-save: POOL truncated");
					g_loadState = LoadState::kRejected;
					continue;
				}
				const bool ok = pool::Table().Deserialize(g_rawPool, [a_intfc](core::FormID a_saved) -> std::optional<core::FormID> {
					RE::FormID current = 0;
					if (!a_intfc->ResolveFormID(a_saved, current)) {
						return std::nullopt;
					}
					return current;
				});
				g_loadState = ok ? LoadState::kLoaded : LoadState::kRejected;
				logger::info("Co-save: POOL {} ({} slots)", ok ? "loaded" : "REJECTED", pool::Table().Used());
			}
			if (blocked) {
				g_loadState = LoadState::kBlocked;
			}
		}

		std::string_view StateName(LoadState a_state)
		{
			switch (a_state) {
			case LoadState::kNone:
				return "none";
			case LoadState::kLoaded:
				return "loaded";
			case LoadState::kRejected:
				return "rejected";
			case LoadState::kBlocked:
				return "blocked";
			}
			return "?";
		}
	}

	void Install()
	{
		auto* serial = SKSE::GetSerializationInterface();
		if (!serial) {
			logger::critical("SKSE serialization interface unavailable");
			return;
		}
		serial->SetUniqueID(kUniqueID);
		serial->SetSaveCallback(OnSave);
		serial->SetLoadCallback(OnLoad);
		serial->SetRevertCallback(OnRevert);
	}

	void OnPostLoad(bool a_loadSucceeded)
	{
		if (!pool::IsReady()) {
			return;
		}
		if (!a_loadSucceeded) {
			logger::warn("kPostLoadGame reported a failed load; nothing reconciled");
			return;
		}
		// A missing table is only safe when the player knows no charge spell: otherwise SCS may still
		// remember staff -> slot bindings that a fresh table would hand to other enchantments.
		const bool knowsPoolSpell = pool::PlayerKnowsAnyPoolSpell();
		const bool block = g_loadState == LoadState::kRejected || g_loadState == LoadState::kBlocked ||
		                   (g_loadState == LoadState::kNone && knowsPoolSpell);
		if (block) {
			pool::Table().Clear();
			conversion::SetBlocked(true, std::format("co-save state {}, player knows charge spells: {}", StateName(g_loadState), knowsPoolSpell));
		} else {
			conversion::SetBlocked(false, "co-save ok");
			pool::RefillFromTable();
		}
		conversion::SetSettled(true);
		conversion::ReconcilePlayer();
		selftest::Report("T-SAVE", !block, std::format("state={} slots={} knowsPoolSpell={} blocked={}", StateName(g_loadState),
											   pool::Table().Used(), knowsPoolSpell, block));
	}

	void OnNewGame()
	{
		conversion::SetBlocked(false, "new game");
		conversion::SetSettled(true);
	}
}
