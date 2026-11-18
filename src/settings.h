#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace scssc
{
	// Data/SKSE/Plugins/SCSSoulCharge.ini. Read once at plugin load.
	struct Settings
	{
		bool selfTest{ false };  // write SELFTEST PASS/FAIL/SKIP lines to the log
		// Appended to the bound spell's name while its staff is held (staff_mark). Empty = off. UTF-8.
		std::string staffMarker{ " \xE2\x98\x85" };  // " ★" (U+2605, in the KS X 1001 set Korean game fonts carry; user choice 2026-11-18)
		// "LocalFormID:Plugin.esp" staves that never take a spell, resolved once the plugins are known.
		std::vector<std::string> excludeSpecs;

		static Settings& Get();
		void             Load();
	};

	// Resolves sExclude entries to runtime FormIDs. Unknown entries are logged and skipped.
	[[nodiscard]] std::unordered_set<RE::FormID> ResolveExcludedStaves(const Settings& a_settings);
}
