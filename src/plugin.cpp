#include "PCH.h"

#include "cast_watch.h"
#include "copy_traits.h"
#include "effect_redirect.h"
#include "frame_hook.h"
#include "hand_slot.h"
#include "main_queue.h"
#include "resource_hook.h"
#include "serialization.h"
#include "spell_copy.h"
#include "settings.h"
#include "staff_art.h"
#include "staff_binding.h"
#include "staff_item.h"
#include "staff_mark.h"

namespace
{
	// The hand-slot preload (11303/11306) and tip-art calls (34150/34152) were read in 1.6.1170 code only (design 13).
	constexpr REL::Version kSupportedRuntime{ 1, 6, 1170, 0 };

	bool g_ready = false;  // every hook in place

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

	// The address checks come first and write nothing. Then the cost hook before anything can put a copy in the hand
	// (without it a copy would drain the staff), and the frame hook, which is what puts the slot back.
	bool InstallHooks()
	{
		if (!scssc::hand_slot::Install() || !scssc::staff_art::Install()) {
			return false;
		}
		if (!scssc::resource_hook::Install() || !scssc::frame_hook::Install()) {
			return false;
		}
		scssc::effect_redirect::Install();
		scssc::copy_traits::Install();
		return true;
	}

	void OnLoadOrNewGame()
	{
		scssc::main_queue::NextGeneration();
		scssc::staff_binding::OnReset();
		scssc::staff_art::Reset();
		scssc::cast_watch::Reset();
		scssc::staff_mark::Reset();
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg || !g_ready) {
			return;
		}
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			scssc::staff_item::OnDataLoaded();
			scssc::cast_watch::InstallEvents();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			OnLoadOrNewGame();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			scssc::staff_binding::InstallMenuHook();  // after SCS hooked the same menu call at its kDataLoaded
			scssc::spell_copy::VerifyAfterLoad();
			break;
		case SKSE::MessagingInterface::kNewGame:
			OnLoadOrNewGame();
			scssc::staff_binding::InstallMenuHook();
			scssc::spell_copy::VerifyAfterLoad();
			break;
		case SKSE::MessagingInterface::kSaveGame:
			scssc::staff_binding::OnSave(scssc::frame_hook::MainThreadId());
			break;
		default:
			break;
		}
	}
}

SKSEPluginInfo(
	.Version = REL::Version{ 0, 2, 0, 0 },
	.Name = "SCSSoulCharge"sv,
	.Author = "qtuna"sv,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitLogging();
	SKSE::Init(a_skse, { .log = false, .trampoline = true, .trampolineSize = 64 });
	scssc::serialization::Install();  // also while off: the saved bindings pass through unchanged
	const auto runtime = REL::Module::get().version();
	logger::info("SCSSoulCharge v0.2.0 loading on runtime {}", runtime.string("."sv));
	if (runtime != kSupportedRuntime) {
		logger::critical("Runtime {} is not supported (only 1.6.1170 so far). Nothing is hooked.", runtime.string("."sv));
		return true;  // stay loaded but inert, so the log explains why nothing happens
	}
	scssc::Settings::Get().Load();
	if (!InstallHooks()) {
		logger::critical("A required hook is missing; the mod stays off (staves work as in the base game)");
		return true;
	}
	auto* messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnMessage)) {
		logger::critical("SKSE messaging interface unavailable; the mod stays off");
		return true;
	}
	g_ready = true;
	scssc::serialization::SetActive(true);
	logger::info("Ready");
	return true;
}
