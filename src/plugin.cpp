#include "PCH.h"

#include "casting.h"
#include "conversion.h"
#include "keywords.h"
#include "pool.h"
#include "serialization.h"
#include "selftest.h"
#include "settings.h"
#include "testcontrols.h"
#include "watch.h"

namespace
{
	constexpr std::array kSupportedRuntimes{
		REL::Version{ 1, 5, 97, 0 },
		REL::Version{ 1, 6, 1170, 0 },
		REL::Version{ 1, 7, 104, 0 },
	};

	void InitLogging()
	{
		auto path = SKSE::log::log_directory();
		if (!path) {
			return;
		}
		*path /= "SCSSoulCharge.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
	}

	bool IsSupportedRuntime(const REL::Version& a_version)
	{
		return std::ranges::find(kSupportedRuntimes, a_version) != kSupportedRuntimes.end();
	}
}

SKSEPluginInfo(
	.Version = REL::Version{ 0, 1, 0, 0 },
	.Name = "SCSSoulCharge"sv,
	.Author = "qtuna"sv,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitLogging();
	SKSE::Init(a_skse, { .log = false, .trampoline = true, .trampolineSize = 64 });

	const auto runtime = REL::Module::get().version();
	logger::info("SCSSoulCharge v0.1.0 loading on runtime {}", runtime.string("."sv));
	if (!IsSupportedRuntime(runtime)) {
		logger::critical("Unsupported runtime {}; supported: 1.5.97, 1.6.1170, 1.7.104. Nothing is hooked.",
			runtime.string("."sv));
		return true;  // stay loaded but inert, so the log explains why nothing happens
	}
	logger::info("Runtime check PASS ({})", REL::Module::IsAE() ? "AE" : "SE");

	scssc::Settings::Get().Load();
	// Cast billing and the frame work must be in place before any staff is turned into an SCS catalyst;
	// otherwise converted staves would cast for free.
	if (!scssc::casting::InstallCastHook() || !scssc::casting::InstallFrameHook()) {
		logger::critical("Cast or frame hook not installed; no staff is converted");
		return true;
	}
	if (!scssc::keywords::Install()) {
		logger::critical("Keyword hook not installed; nothing is converted");
		return true;
	}

	scssc::serialization::Install();

	auto* messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
			if (!a_msg) {
				return;
			}
			switch (a_msg->type) {
			case SKSE::MessagingInterface::kDataLoaded:
				{
					const auto& stats = scssc::keywords::GetStats();
					logger::info("kDataLoaded: staves={} converted={} skipped={} scriptedSeenAtInitItem={} ready={}",
						stats.staves, stats.converted, stats.skipped, stats.scriptedSeen, stats.ready);
					if (stats.ready && scssc::pool::IsReady()) {
						scssc::conversion::Install();
					}
					scssc::selftest::RunDataLoaded();
					scssc::watch::OnDataLoaded();
					scssc::testcontrols::Install();
					break;
				}
			case SKSE::MessagingInterface::kPostLoadGame:
				scssc::serialization::OnPostLoad(a_msg->data != nullptr);
				break;
			case SKSE::MessagingInterface::kNewGame:
				scssc::serialization::OnNewGame();
				break;
			default:
				break;
			}
		})) {
		logger::critical("SKSE messaging interface unavailable");
	}
	return true;
}
