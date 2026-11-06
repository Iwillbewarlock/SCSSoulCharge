#include "PCH.h"

#include "serialization.h"

#include "conversion.h"
#include "pool.h"
#include "selftest.h"

namespace scssc::serialization
{
	namespace
	{
		constexpr std::uint32_t kUniqueID = 'SCSC';  // SCS itself uses 'SCSF'
		constexpr std::uint32_t kPoolRecord = 'POOL';
		constexpr std::uint32_t kPoolRecordVersion = 1;
		constexpr std::uint32_t kMaxRecordBytes = 8 + pool::kPoolSize * 8;

		enum class LoadState
		{
			kNone,       // no POOL record seen in this load
			kLoaded,
			kRejected,
		};
		LoadState g_loadState = LoadState::kNone;

		void OnSave(SKSE::SerializationInterface* a_intfc)
		{
			const auto bytes = pool::Table().Serialize();
			if (!a_intfc->WriteRecord(kPoolRecord, kPoolRecordVersion, bytes.data(), static_cast<std::uint32_t>(bytes.size()))) {
				logger::error("Co-save: writing POOL ({} bytes) failed", bytes.size());
				return;
			}
			logger::info("Co-save: POOL saved ({} slots)", pool::Table().Used());
		}

		void OnRevert(SKSE::SerializationInterface*)
		{
			pool::Clear();
			g_loadState = LoadState::kNone;
			conversion::SetBlocked(false, "revert");
		}

		void OnLoad(SKSE::SerializationInterface* a_intfc)
		{
			std::uint32_t type = 0;
			std::uint32_t version = 0;
			std::uint32_t length = 0;
			while (a_intfc->GetNextRecordInfo(type, version, length)) {
				if (type != kPoolRecord) {
					logger::warn("Co-save: unknown record {:08X} skipped", type);
					continue;
				}
				if (version != kPoolRecordVersion || length > kMaxRecordBytes) {
					logger::error("Co-save: POOL version {} length {} not supported", version, length);
					g_loadState = LoadState::kRejected;
					continue;
				}
				std::vector<std::uint8_t> bytes(length);
				if (a_intfc->ReadRecordData(bytes.data(), length) != length) {
					logger::error("Co-save: POOL truncated");
					g_loadState = LoadState::kRejected;
					continue;
				}
				const bool ok = pool::Table().Deserialize(bytes, [a_intfc](core::FormID a_saved) -> std::optional<core::FormID> {
					RE::FormID current = 0;
					if (!a_intfc->ResolveFormID(a_saved, current)) {
						return std::nullopt;
					}
					return current;
				});
				g_loadState = ok ? LoadState::kLoaded : LoadState::kRejected;
				logger::info("Co-save: POOL {} ({} slots)", ok ? "loaded" : "REJECTED", pool::Table().Used());
			}
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
		const bool knowsPoolSpell = pool::PlayerKnowsAnyPoolSpell();
		if (g_loadState == LoadState::kRejected || (g_loadState == LoadState::kNone && knowsPoolSpell)) {
			pool::Table().Clear();
			conversion::SetBlocked(true, g_loadState == LoadState::kRejected ? "co-save POOL record rejected" :
																			  "co-save POOL record missing but the player knows charge spells");
			selftest::Report("T-SAVE", false, std::format("loadSucceeded={} state={} knowsPoolSpell={}", a_loadSucceeded,
												  static_cast<int>(g_loadState), knowsPoolSpell));
			return;
		}
		pool::RefillFromTable();
		conversion::ReconcilePlayer();
		selftest::Report("T-SAVE", true, std::format("loadSucceeded={} slots={} state={}", a_loadSucceeded, pool::Table().Used(),
											 static_cast<int>(g_loadState)));
	}

	void OnNewGame()
	{
		conversion::SetBlocked(false, "new game");
	}
}
