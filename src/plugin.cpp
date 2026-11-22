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
#include "runtime_check.h"

#include <atomic>

#include <Windows.h>

namespace
{
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

	// The address checks come first and write nothing; all of them run, so a log from a game build nobody here runs
	// shows every failure at once (reviewer 2026-11-22). Then the cost hook before anything can put a copy in the hand
	// (without it a copy would drain the staff), and the frame hook, which is what puts the slot back.
	bool InstallHooks()
	{
		const bool hand = scssc::hand_slot::Install();
		const bool art = scssc::staff_art::Install();
		const bool frame = scssc::frame_hook::Prepare();
		const bool resource = scssc::resource_hook::Prepare();
		const bool vtables = scssc::runtime_check::VtableSlotsOk();
		if (!(hand && art && frame && resource && vtables)) {
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
	.Version = REL::Version{ 1, 0, 1, 0 },
	.Name = "SCSSoulCharge"sv,
	.Author = "qtuna"sv,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitLogging();
	// SKSE before 2.0.18 (SE) has no trampoline interface and no GetPluginInfo, which SKSE::Init reads and the hooks
	// need (skse64_whatsnew.txt 2.0.18). Checked before Init touches them (Astra 2026-11-22).
	if (const auto skse = REL::Version::unpack(a_skse->SKSEVersion()); skse < REL::Version{ 2, 0, 18, 0 }) {
		logger::critical("SKSE {} is too old (2.0.18 or later needed). Nothing is hooked.", skse.string("."sv));
		::MessageBoxW(nullptr, L"SCSSoulCharge is off: SKSE 2.0.18 or later is needed.", L"SCSSoulCharge", MB_OK | MB_ICONWARNING);
		return true;
	}
	SKSE::Init(a_skse, { .log = false, .trampoline = true, .trampolineSize = 64 });
	scssc::serialization::Install();  // also while off: the saved bindings pass through unchanged
	scssc::Settings::Get().Load();    // reads files only: safe on any runtime, and the off notice needs its text
	scssc::messages::Load();
	ApplyLogSettings();
	const auto runtime = REL::Module::get().version();
	const auto* declaration = SKSE::PluginDeclaration::GetSingleton();
	logger::info("SCSSoulCharge v{} loading on runtime {}", declaration ? declaration->GetVersion().string("."sv) : "?"s, runtime.string("."sv));
	if (!scssc::runtime_check::SupportedRuntime(runtime)) {
		logger::critical("Runtime {} is not supported (1.5.97, or 1.6.317 up to 1.7.x). Nothing is hooked.", runtime.string("."sv));
		const auto notice = scssc::messages::Text(scssc::messages::Msg::kOffRuntime, runtime.string("."sv));
		const auto wide = SKSE::stl::utf8_to_utf16(notice).value_or(L"SCSSoulCharge is off: this game version is not supported.");
		::MessageBoxW(nullptr, wide.c_str(), L"SCSSoulCharge", MB_OK | MB_ICONWARNING);
		return true;  // stay loaded but inert, so the log and the notice explain why nothing happens
	}
	g_supportedRuntime = true;
	if (!InstallHooks()) {
		logger::critical("A start-up check or a hook failed (T-RUNTIME lines above); the mod stays off (staves work as in the base game)");
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
