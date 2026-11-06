#include "SpellCastingStaff/Controller.h"
#ifndef SCS_CLIB_17104_BACKPORT
#error Build with the bundled CommonLibSSE-NG 3.6.0 SCS backport.
#endif
#include "SpellCastingStaff/RuntimeCompatibility.h"
#include "SpellCastingStaff/FrameHook.h"
#include "SpellCastingStaff/AttackHook.h"
#include "SpellCastingStaff/BoundLifecycleHook.h"
#include "SpellCastingStaff/MenuEquipHook.h"
#include "SpellCastingStaff/CompatibilityError.h"
#include "SpellCastingStaff/LogPath.h"
#include "SpellCastingStaff/PerkEntryProbe.h"
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <exception>
#include "SpellCastingStaff/Config.h"

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    if (!SpellCastingStaff::RuntimeCompatibility::Supported(skse->RuntimeVersion())) return false;
    SKSE::Init(skse);
    // Some CommonLibSSE-NG versions resolve AE 1.6.1170 logs under the
    // unintended "My Games\\Skyrim.INI\\SKSE" directory. Use the OS Documents
    // known folder directly so the destination is stable on SE and AE.
    const auto logDirectory = SpellCastingStaff::LogPath::ResolveDirectory();
    if (!logDirectory) {
        ::MessageBoxW(nullptr,
            L"SpellCastingStaff cannot locate or create its SKSE log folder under\n"
            L"Documents\\My Games\\Skyrim Special Edition\\SKSE.\n\n"
            L"Check Documents folder permissions and try again.",
            L"SpellCastingStaff - Logging Error", MB_OK | MB_ICONERROR | MB_TASKMODAL);
        return false; // Do not load silently without a diagnostic log.
    }
    const auto runtime = skse->RuntimeVersion();
    const auto fileName = SpellCastingStaff::LogPath::FileNameForRuntime(
        runtime.major(), runtime.minor(), runtime.patch(), runtime.build());
    if (fileName.empty()) return false; // Defensive guard; Supported() ran above.
    const auto logPath = *logDirectory / fileName;
    try {
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            logPath.string(), 4 * 1024 * 1024, 3, true);
        auto logger = std::make_shared<spdlog::logger>("SpellCastingStaff", std::move(sink));
        spdlog::set_default_logger(std::move(logger));
    } catch (const std::exception&) {
        ::MessageBoxW(nullptr,
            L"SpellCastingStaff could not open its log file in\n"
            L"Documents\\My Games\\Skyrim Special Edition\\SKSE.\n\n"
            L"Check folder permissions, free disk space, and antivirus protection.",
            L"SpellCastingStaff - Logging Error", MB_OK | MB_ICONERROR | MB_TASKMODAL);
        return false;
    }
    // The same filename must appear in the in-game compatibility dialog.
    SpellCastingStaff::CompatibilityError::SetLogFileName(fileName);
    // Default to WARNING/ERROR; INFO only with bVerboseLog=1.
    const bool verbose = SpellCastingStaff::GetConfig().verbose;
    spdlog::set_level(verbose ? spdlog::level::info : spdlog::level::warn);
    spdlog::flush_on(verbose ? spdlog::level::info : spdlog::level::warn);
    SKSE::log::warn("Log file: {}", logPath.string());

    // Always emit this at WARNING level even with bVerboseLog=0 so a stale
    // deployed DLL can be identified unambiguously from the latest log.
    SKSE::log::warn("SpellCastingStaff v2.1.1 (SE 1.5.97 / Steam 1.6.1170 / Steam 1.7.104)");
    SKSE::log::info("VAV08 automatically enabled (all spells; no AV writes)");
    SKSE::log::warn("DUAL F&F PROJECTILE enabled={} (disable with bExperimentalProjectileDual=0)",
                    SpellCastingStaff::GetConfig().experimentalProjectileDual);
    SKSE::log::warn("Runtime {}", skse->RuntimeVersion().string());
    // No global native CALL/vtable preflight: each hook checks its own site.
    // Keep the narrow hard gate for runtime and AE Address Library identity.
    if (!SpellCastingStaff::RuntimeCompatibility::ValidateRuntimeAndMappings()) return false;
    // One shared SKSE trampoline for four independent write_call<5> sites.
    // Do not call AllocTrampoline again in either hook installer.
    SKSE::AllocTrampoline(128);
    // Install the native BoundItemEffect restore-slot hook only after the
    // plugin forms have been resolved at kDataLoaded below.
    // Frame hook is installed once at plugin load; no task-queue recursion.
    if (!SpellCastingStaff::FrameHook::Install()) {
        // Nothing has been patched yet: safe to fail the SKSE load without
        // leaving a thunk in an unloaded DLL. This hook is mandatory.
        SKSE::log::critical("Required frame hook unavailable; refusing plugin load");
        return false;
    }
    SpellCastingStaff::Controller::Get().SetFrameHookReady(true);
    if (auto* serial = SKSE::GetSerializationInterface()) {
        serial->SetUniqueID(SpellCastingStaff::kSerializationID);
        serial->SetSaveCallback(
            [](SKSE::SerializationInterface* api) { SpellCastingStaff::Controller::Get().Save(api); });
        serial->SetLoadCallback(
            [](SKSE::SerializationInterface* api) { SpellCastingStaff::Controller::Get().Load(api); });
        serial->SetRevertCallback(
            [](SKSE::SerializationInterface*) { SpellCastingStaff::Controller::Get().OnRevert(); });
    }
    if (auto* messaging = SKSE::GetMessagingInterface()) {
        messaging->RegisterListener([](SKSE::MessagingInterface::Message* message) {
            if (!message) {
                return;
            }
            auto& controller = SpellCastingStaff::Controller::Get();
            switch (message->type) {
                case SKSE::MessagingInterface::kInputLoaded:
                    SpellCastingStaff::AttackHook::Install();
                    controller.AttachInput();
                    break;
                case SKSE::MessagingInterface::kDataLoaded:
                    if (controller.ResolveForms()) {
                        SpellCastingStaff::PerkEntryProbe::Install();
                        if (!SpellCastingStaff::BoundLifecycleHook::Install()) {
                            SKSE::log::error("BOUND RESTORE unavailable; original Skyrim equip behavior retained");
                        }
                        controller.Attach();
                        SpellCastingStaff::AttackHook::Install();
                        controller.AttachInput();
                        if (!SpellCastingStaff::MenuEquipHook::Install()) {
                            // SKSEPlugin_Load has already returned; we cannot safely
                            // claim success or retroactively fail plugin load here.
                            // Disable native staff casts instead of silently running
                            // with no way to assign a staff spell.
                            controller.DisableForCompatibilityFailure();
                            SpellCastingStaff::CompatibilityError::MarkMenuHookFailed();
                        }
                    }
                    break;
                case SKSE::MessagingInterface::kNewGame:
                    SpellCastingStaff::PerkEntryProbe::ResetCounters();
                    controller.OnNewGame();
                    SpellCastingStaff::CompatibilityError::OnGameReady();
                    if (!controller.HasCompatibilityFailure()) SpellCastingStaff::PerkEntryProbe::Arm();
                    break;
                case SKSE::MessagingInterface::kPostLoadGame:
                    SpellCastingStaff::PerkEntryProbe::ResetCounters(true); // preserve F6 across save loads
                    controller.OnPostLoadGame();
                    SpellCastingStaff::CompatibilityError::OnGameReady();
                    if (!controller.HasCompatibilityFailure()) SpellCastingStaff::PerkEntryProbe::Arm();
                    break;
                default:
                    break;
            }
        });
    }
    return true;
}
