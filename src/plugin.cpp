#include "PCH.h"

#include "cast_watch.h"
#include "copy_traits.h"
#include "effect_redirect.h"
#include "frame_hook.h"
#include "hand_slot.h"
#include "main_queue.h"
#include "messages.h"
#include "plugin_state.h"
#include "resource_hook.h"
#include "serialization.h"
#include "spell_copy.h"
#include "settings.h"
#include "staff_art.h"
#include "staff_binding.h"
#include "staff_item.h"
#include "staff_mark.h"

#include <atomic>

#include <Windows.h>

namespace
{
	// The hand-slot preload (11303/11306) and tip-art calls (34150/34152) were read in 1.6.1170 code only (design 13).
	constexpr REL::Version kSupportedRuntime{ 1, 6, 1170, 0 };

	std::atomic<bool> g_ready{ false };  // every hook in place
	std::string       g_offNotice;       // why the mod is off, shown once in game (final review 2026-11-19)
	bool              g_supportedRuntime = false;

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
		log->flush_on(spdlog::level::info);  // info lines are events (pick, save, load) now; the per-cast lines are debug
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
	}

	// The self-test reads the per-cast lines (debug level), flushed like the rest.
	void ApplyLogSettings()
	{
		if (scssc::Settings::Get().selfTest) {
			spdlog::set_level(spdlog::level::debug);
			spdlog::flush_on(spdlog::level::debug);
		}
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

	// Only on the supported runtime (a hook failed): the game's message box needs Address Library ids, which an
	// unsupported runtime may not have; that case gets a Windows message box at plugin load instead (Opus 2026-11-19).
	void ShowOffNoticeOnce()
	{
		static bool shown = false;
		if (!shown && !g_offNotice.empty()) {
			shown = true;
			RE::DebugMessageBox(g_offNotice.c_str());
		}
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		if (!g_ready) {
			// Off, the listener is there only to say so in game: the log alone left players guessing (Opus 2026-11-19).
			if (g_supportedRuntime && (a_msg->type == SKSE::MessagingInterface::kPostLoadGame || a_msg->type == SKSE::MessagingInterface::kNewGame)) {
				ShowOffNoticeOnce();
				if (a_msg->type == SKSE::MessagingInterface::kPostLoadGame && scssc::Settings::Get().restoreNames) {
					scssc::staff_binding::RunUninstallNow();  // removing the mod is most likely when it is off (Opus 2026-11-19)
				}
			}
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
			scssc::staff_binding::OnPostLoad();
			break;
		case SKSE::MessagingInterface::kNewGame:
			OnLoadOrNewGame();
			scssc::staff_binding::Table().Clear();  // SKSE's revert callback should have done it; harmless if so (Opus 2026-11-19)
			scssc::staff_binding::InstallMenuHook();
			scssc::spell_copy::VerifyAfterLoad();
			scssc::staff_binding::OnPostLoad();
			break;
		case SKSE::MessagingInterface::kSaveGame:
			scssc::staff_binding::OnSave(scssc::frame_hook::MainThreadId());
			break;
		default:
			break;
		}
	}

	void RegisterListener()
	{
		auto* messaging = SKSE::GetMessagingInterface();
		if (!messaging || !messaging->RegisterListener(OnMessage)) {
			logger::critical("SKSE messaging interface unavailable");
		}
	}
}

namespace scssc
{
	bool IsActive()
	{
		// Also off for other plugins when the menu hook failed or the uninstall option runs: nothing is bound then (Astra 2026-11-19).
		return g_ready.load() && staff_binding::Applying();
	}
}

SKSEPluginInfo(
	.Version = REL::Version{ 1, 0, 0, 0 },
	.Name = "SCSSoulCharge"sv,
	.Author = "qtuna"sv,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitLogging();
	SKSE::Init(a_skse, { .log = false, .trampoline = true, .trampolineSize = 64 });
	scssc::serialization::Install();  // also while off: the saved bindings pass through unchanged
	scssc::Settings::Get().Load();    // reads files only: safe on any runtime, and the off notice needs its text
	scssc::messages::Load();
	ApplyLogSettings();
	const auto runtime = REL::Module::get().version();
	logger::info("SCSSoulCharge v1.0.0 loading on runtime {}", runtime.string("."sv));
	if (runtime != kSupportedRuntime) {
		logger::critical("Runtime {} is not supported (only 1.6.1170 so far). Nothing is hooked.", runtime.string("."sv));
		const auto notice = scssc::messages::Text(scssc::messages::Msg::kOffRuntime, runtime.string("."sv));
		const auto wide = SKSE::stl::utf8_to_utf16(notice).value_or(L"SCSSoulCharge is off: this game version is not supported.");
		::MessageBoxW(nullptr, wide.c_str(), L"SCSSoulCharge", MB_OK | MB_ICONWARNING);
		return true;  // stay loaded but inert, so the log and the notice explain why nothing happens
	}
	g_supportedRuntime = true;
	if (!InstallHooks()) {
		logger::critical("A required hook is missing; the mod stays off (staves work as in the base game)");
		g_offNotice = scssc::messages::Text(scssc::messages::Msg::kOffHook);
		RegisterListener();
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
