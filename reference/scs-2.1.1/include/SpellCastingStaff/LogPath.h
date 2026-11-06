#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace SpellCastingStaff::LogPath {

// Keep logs from different supported runtimes separate.  Exact builds only:
// all other versions are rejected by RuntimeCompatibility::Supported().
[[nodiscard]] constexpr std::string_view FileNameForRuntime(
    std::uint16_t major, std::uint16_t minor, std::uint16_t patch, std::uint16_t build) noexcept {
    if (major == 1 && minor == 5 && patch == 97 && build == 0) return "SpellCastingStaff_1.5.97.log";
    if (major == 1 && minor == 6 && patch == 1170 && build == 0) return "SpellCastingStaff_1.6.1170.log";
    if (major == 1 && minor == 7 && patch == 104 && build == 0) return "SpellCastingStaff_1.7.104.log";
    return {};  // Fail closed; caller has already checked Supported().
}

// Pure path construction: respects redirected/OneDrive Documents and does not
// consult the affected CommonLibSSE/SKSE log_directory() implementation.
[[nodiscard]] inline std::filesystem::path FromDocuments(const std::filesystem::path& documents) {
    return documents / "My Games" / "Skyrim Special Edition" / "SKSE";
}

// Resolve Windows' actual Documents known folder and create the SKSE log dir.
[[nodiscard]] std::optional<std::filesystem::path> ResolveDirectory();

}  // namespace SpellCastingStaff::LogPath
