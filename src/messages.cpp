#include "PCH.h"

#include "messages.h"

#include <array>

#include <Windows.h>

namespace scssc::messages
{
	namespace
	{
		constexpr auto  kFilePath = L"Data\\SKSE\\Plugins\\SCSSoulCharge_Messages.ini";
		constexpr DWORD kLineBufferSize = 1024;

		struct Line
		{
			const wchar_t* key;
			const char*    english;
			int            names;  // how many names the game gives this line: {0}, or {0} and {1}
		};

		constexpr std::array<Line, static_cast<std::size_t>(Msg::kCount)> kLines{ {
			{ L"sPickDropped", "The staff changed before the spell could be set; pick it again", 0 },
			{ L"sSingleOnly", "Only a single staff can hold a spell ({0} share this item)", 1 },
			{ L"sNameLocked", "This staff's name belongs to a quest; it cannot hold a spell", 0 },
			{ L"sNowCasts", "{0} now casts {1}", 2 },
			{ L"sOwnAgain", "{0} casts its own enchantment again", 1 },
			{ L"sCannotBind", "{0} cannot go on a staff: {1}", 2 },
			{ L"sReasonNotASpell", "not a spell (power, shout or ability)", 0 },
			{ L"sReasonRuntime", "made by a mod during play", 0 },
			{ L"sReasonNotKnown", "you do not know it", 0 },
			{ L"sReasonCastingType", "only fire-and-forget and concentration spells", 0 },
			{ L"sReasonNoEffects", "it has no effects", 0 },
			{ L"sDormant", "{0} casts its own enchantment until its spell is known again", 1 },
			{ L"sSpellGone", "{0}: its spell no longer exists; it casts its own enchantment again", 1 },
			{ L"sTableFull", "Too many staves hold spells ({0}); clear one first", 1 },
			{ L"sOffRuntime", "SCSSoulCharge is off: game version {0} is not supported (1.6.1170 only). Staves work as in the base game.", 1 },
			{ L"sOffHook", "SCSSoulCharge is off: another mod changed a place it needs. Staves work as in the base game. See SCSSoulCharge.log.", 0 },
			{ L"sMenuHookFailed",
				"SCSSoulCharge: another mod changed the magic menu. Spells cannot be put on staves, and bound staves cast their own enchantment. See SCSSoulCharge.log.", 0 },
			{ L"sUninstallDone", "SCSSoulCharge: {0} carried staves got their names back and lost their spells (staves elsewhere keep theirs). Save now, then remove the mod.", 1 },
		} };

		std::array<std::string, kLines.size()> g_text = [] {
			std::array<std::string, kLines.size()> out;
			for (std::size_t i = 0; i < kLines.size(); ++i) {
				out[i] = kLines[i].english;
			}
			return out;
		}();

		std::array<std::string, kLines.size()> g_english = g_text;

		// Formats a_line with exactly the names the game gives it (always text). Empty on a format error.
		std::optional<std::string> TryFormat(const std::string& a_line, int a_names, const std::string& a_first, const std::string& a_second)
		{
			try {
				switch (a_names) {
				case 0:
					return std::vformat(a_line, std::make_format_args());
				case 1:
					return std::vformat(a_line, std::make_format_args(a_first));
				default:
					return std::vformat(a_line, std::make_format_args(a_first, a_second));
				}
			} catch (const std::format_error&) {
				return std::nullopt;
			} catch (const std::length_error&) {  // a translated line asking for a huge width (Opus 2026-11-19)
				return std::nullopt;
			} catch (const std::bad_alloc&) {
				return std::nullopt;
			}
		}

		// A line that formats with sample names of its own count formats in game: every name is passed as text.
		bool Formats(const std::string& a_line, int a_names)
		{
			return TryFormat(a_line, a_names, "A", "B").has_value();
		}
	}

	std::string Text(Msg a_msg, const std::string& a_first, const std::string& a_second)
	{
		const auto index = static_cast<std::size_t>(a_msg);
		if (auto text = TryFormat(g_text[index], kLines[index].names, a_first, a_second)) {
			return *std::move(text);
		}
		// Load() checked the line; this is the last guard. The English line is formatted with the same names (Astra 2026-11-19).
		return TryFormat(g_english[index], kLines[index].names, a_first, a_second).value_or(g_english[index]);
	}

	void Load()
	{
		std::size_t taken = 0;
		for (std::size_t i = 0; i < kLines.size(); ++i) {
			std::wstring buffer(kLineBufferSize, L'\0');
			const auto   length = ::GetPrivateProfileStringW(L"Messages", kLines[i].key, L"", buffer.data(), kLineBufferSize, kFilePath);
			buffer.resize(length);
			if (buffer.empty()) {
				continue;
			}
			const auto text = SKSE::stl::utf16_to_utf8(buffer);
			const auto key = SKSE::stl::utf16_to_utf8(kLines[i].key).value_or("?");
			if (!text || !Formats(*text, kLines[i].names)) {
				logger::warn("Messages: {} = '{}' does not format with {} name(s) (the English line shows which); the English line is kept", key,
					text.value_or("?"), kLines[i].names);
				continue;
			}
			g_text[i] = *text;
			++taken;
		}
		logger::info("Messages: {} of {} lines from SCSSoulCharge_Messages.ini", taken, kLines.size());
	}

	const std::string& Raw(Msg a_msg)
	{
		return g_text[static_cast<std::size_t>(a_msg)];
	}

	const std::string& English(Msg a_msg)
	{
		return g_english[static_cast<std::size_t>(a_msg)];
	}

	Msg ReasonFor(core::BindVerdict a_verdict)
	{
		switch (a_verdict) {
		case core::BindVerdict::kNotASpell:
			return Msg::kReasonNotASpell;
		case core::BindVerdict::kRuntimeForm:
			return Msg::kReasonRuntime;
		case core::BindVerdict::kNotKnown:
			return Msg::kReasonNotKnown;
		case core::BindVerdict::kCastingType:
			return Msg::kReasonCastingType;
		default:
			return Msg::kReasonNoEffects;  // the only other verdict reachable with the copy keeping spell values
		}
	}
}
