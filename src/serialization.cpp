#include "PCH.h"

#include "serialization.h"

#include "frame_hook.h"
#include "selftest.h"
#include "staff_binding.h"

namespace scssc::serialization
{
	namespace
	{
		constexpr std::uint32_t kUniqueID = 'SCSC';  // SCS itself uses 'SCSF'
		constexpr std::uint32_t kBindRecord = 'BIND';
		constexpr std::uint32_t kBindRecordVersion = 1;
		constexpr std::uint32_t kMaxRecordBytes = 4u << 20;  // far above any real table (about 1.1 KB per binding at most)

		// While the mod is off (unsupported runtime, a hook missing) the table is still read and written, so one save
		// made in that state keeps every binding (reviewer 2026-11-17). Resolved, not passed through as raw bytes: SKSE
		// writes the new save's plugin indices, so raw FormIDs would point at other forms after a load order change
		// (Codex r2, skse64 InternalSerialization.cpp). Off, nothing else touches the table.
		std::atomic<bool> g_active{ false };

		void CheckThread(const char* a_where)
		{
			const auto main = frame_hook::MainThreadId();
			const auto thread = REX::W32::GetCurrentThreadId();
			selftest::Report("T-THREAD", main == 0 || thread == main, std::format("{} on thread {} (frame hook {})", a_where, thread, main));
		}

		void OnSave(SKSE::SerializationInterface* a_intfc)
		{
			if (g_active.load()) {
				CheckThread("co-save write");
			}
			const auto& table = staff_binding::TableForSave();
			const auto  bytes = table.Serialize();
			if (!a_intfc->WriteRecord(kBindRecord, kBindRecordVersion, bytes.data(), static_cast<std::uint32_t>(bytes.size()))) {
				logger::error("Co-save: writing BIND ({} bytes) failed; this save keeps no staff bindings", bytes.size());
				return;
			}
			logger::info("Co-save: BIND saved ({} bindings, {} bytes)", table.Size(), bytes.size());
		}

		void OnRevert(SKSE::SerializationInterface*)
		{
			staff_binding::Table().Clear();
		}

		void OnLoad(SKSE::SerializationInterface* a_intfc)
		{
			std::uint32_t type = 0;
			std::uint32_t version = 0;
			std::uint32_t length = 0;
			while (a_intfc->GetNextRecordInfo(type, version, length)) {
				if (type != kBindRecord) {
					logger::info("Co-save: record {:08X} from an older test build skipped", type);
					continue;
				}
				if (version != kBindRecordVersion || length > kMaxRecordBytes) {
					logger::error("Co-save: BIND version {} length {} not supported; staff bindings start empty", version, length);
					selftest::Report("T-SAVE", false, std::format("BIND version {} length {}", version, length));
					continue;
				}
				std::vector<std::uint8_t> raw(length);
				if (a_intfc->ReadRecordData(raw.data(), length) != length) {
					logger::error("Co-save: BIND truncated; staff bindings start empty");
					selftest::Report("T-SAVE", false, "BIND truncated");
					continue;
				}
				if (g_active.load()) {
					CheckThread("co-save read");
				}
				std::size_t dropped = 0;
				std::size_t orphaned = 0;
				auto&       table = staff_binding::Table();
				const bool  ok = table.Deserialize(
                    raw,
                    [a_intfc](core::FormID a_saved) -> std::optional<core::FormID> {
                        RE::FormID current = 0;
                        if (!a_intfc->ResolveFormID(a_saved, current)) {
                            return std::nullopt;
                        }
                        return current;
                    },
                    &dropped, &orphaned);
				logger::info("Co-save: BIND {} ({} bindings, {} dropped with their staff's plugin, {} whose spell's plugin is gone)", ok ? "loaded" : "REJECTED",
					table.Size(), dropped, orphaned);
				selftest::Report("T-SAVE", ok, std::format("bindings={} dropped={} orphaned={}", table.Size(), dropped, orphaned));
				staff_binding::OnLoaded(ok ? table.Size() : 0);
			}
		}
	}

	void Install()
	{
		auto* serial = SKSE::GetSerializationInterface();
		if (!serial) {
			logger::critical("SKSE serialization interface unavailable; staff bindings are not saved");
			return;
		}
		serial->SetUniqueID(kUniqueID);
		serial->SetSaveCallback(OnSave);
		serial->SetLoadCallback(OnLoad);
		serial->SetRevertCallback(OnRevert);
	}

	void SetActive(bool a_active)
	{
		g_active = a_active;
	}
}
