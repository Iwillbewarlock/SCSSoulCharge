#pragma once
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <istream>
#include <string>
#include <string_view>

namespace SpellCastingStaff {
// Immutable after first read: restarting Skyrim applies INI changes.
struct Config {
    bool verbose = false;
    // DirectInput keyboard scan code; 64 (0x40) = F6; 0 disables the diagnostic shortcut.
    std::uint32_t diagnosticHotkey = 0;
    // Experimental only. Set to 0 to disable all projectile flag writes.
    bool experimentalProjectileDual = true;

    bool showSpellSelectedMessage = true;
    bool showSpellClearedMessage = true;
    bool showMissingSpellMessage = true;
    bool showInsufficientMagickaMessage = true;
    std::string spellSelectedMessage = "{staff}: {spell} selected.";
    std::string spellClearedMessage = "{staff}: {spell} assignment cleared.";
    std::string missingSpellMessage = "Assign a spell to your Spellcasting Staff first.";
    std::string insufficientMagickaMessage = "Not enough Magicka.";
    std::string insufficientStaminaMessage = "Not enough Stamina.";
};

inline std::string_view TrimConfig(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// Numeric keys accept an inline comment; message values preserve literal # and ;.
inline bool ParseConfigBool(std::string_view value, bool& output) {
    const auto comment = value.find_first_of(";#");
    if (comment != std::string_view::npos) value = value.substr(0, comment);
    value = TrimConfig(value);
    if (value == "0" || value == "1") {
        output = value == "1";
        return true;
    }
    return false;
}

// Only keyboard DirectInput scan codes are accepted: 0 disables the shortcut;
// 1..255 selects a key. Accept decimal or 0x-prefixed hexadecimal values.
// Invalid or overflowing input leaves the previously selected value unchanged.
inline bool ParseDiagnosticHotkey(std::string_view value, std::uint32_t& output) {
    const auto comment = value.find_first_of(";#");
    if (comment != std::string_view::npos) value = value.substr(0, comment);
    value = TrimConfig(value);
    int base = 10;
    if (value.starts_with("0x") || value.starts_with("0X")) {
        value.remove_prefix(2);
        base = 16;
    }
    if (value.empty()) return false;
    std::uint32_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed, base);
    if (error != std::errc{} || end != value.data() + value.size() || parsed > 255) return false;
    output = parsed;
    return true;
}

// Skyrim UI receives an ordinary zero-terminated UTF-8 string. Keep untrusted
// INI text bounded and reject control bytes (including embedded NUL).
inline bool ValidConfigMessage(std::string_view value) {
    if (value.size() > 512) return false;
    for (unsigned char ch : value) {
        if (ch < 0x20 || ch == 0x7F) return false;
    }
    return true; // An empty message is intentional; bShow* is the preferred toggle.
}

inline Config ParseConfig(std::istream& input) {
    Config result;
    for (std::string line; std::getline(input, line);) {
        if (line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
        const auto trimmed = TrimConfig(line);
        if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#') continue;
        const auto equal = line.find('=');
        if (equal == std::string::npos) continue;
        const auto key = TrimConfig(std::string_view(line).substr(0, equal));
        const auto value = TrimConfig(std::string_view(line).substr(equal + 1));
        if (key == "bVerboseLog") ParseConfigBool(value, result.verbose);
        else if (key == "iDiagnosticHotkey") ParseDiagnosticHotkey(value, result.diagnosticHotkey);
        else if (key == "bExperimentalProjectileDual") ParseConfigBool(value, result.experimentalProjectileDual);
        else if (key == "bShowSpellSelectedMessage") ParseConfigBool(value, result.showSpellSelectedMessage);
        else if (key == "bShowSpellClearedMessage") ParseConfigBool(value, result.showSpellClearedMessage);
        else if (key == "bShowMissingSpellMessage") ParseConfigBool(value, result.showMissingSpellMessage);
        else if (key == "bShowInsufficientMagickaMessage") ParseConfigBool(value, result.showInsufficientMagickaMessage);
        else if (key == "sSpellSelectedMessage" && ValidConfigMessage(value)) result.spellSelectedMessage = value;
        else if (key == "sSpellClearedMessage" && ValidConfigMessage(value)) result.spellClearedMessage = value;
        else if (key == "sMissingSpellMessage" && ValidConfigMessage(value)) result.missingSpellMessage = value;
        else if (key == "sInsufficientMagickaMessage" && ValidConfigMessage(value)) result.insufficientMagickaMessage = value;
        else if (key == "sInsufficientStaminaMessage" && ValidConfigMessage(value)) result.insufficientStaminaMessage = value;
    }
    return result;
}

inline const Config& GetConfig() {
    static const Config config = [] {
        std::ifstream input("Data/SKSE/Plugins/SpellCastingStaff.ini", std::ios::binary);
        return ParseConfig(input);
    }();
    return config;
}

// Literal replacement, NOT printf/fmt: translation contents and spell names
// cannot be evaluated as format strings. Translators may move/repeat {spell}.
// No substitutions happen inside spellName itself.
inline std::string FormatSpellSelectedMessage(std::string_view message,
                                               std::string_view spellName,
                                               std::string_view staffName = "Spellcasting Staff") {
    constexpr std::size_t maxOutputBytes = 2048;
    std::string out;
    for (std::size_t pos = 0; pos < message.size();) {
        std::string_view piece;
        if (message.substr(pos).starts_with("{spell}")) { piece = spellName; pos += 7; }
        else if (message.substr(pos).starts_with("{staff}")) { piece = staffName; pos += 7; }
        else { piece = message.substr(pos++, 1); }
        if (piece.size() > maxOutputBytes - out.size()) return {};
        out.append(piece);
    }
    return out;
}
} // namespace SpellCastingStaff
