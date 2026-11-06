#pragma once

#include "core/staff_filter.h"

#include <string>
#include <vector>

namespace scssc
{
	// Data/SKSE/Plugins/SCSSoulCharge.ini. Read once at plugin load, before any form is initialised.
	struct Settings
	{
		int  basicTier{ 0 };            // SCS Basic tier keyword given to converted staves (0..5)
		bool keepStaffNames{ true };    // undo SCS renaming the base staff to the chosen spell
		bool selfTest{ false };         // write SELFTEST PASS/FAIL lines to the log
		bool testControls{ false };     // development: F9/F10/F11 test controls (see testcontrols.h)
		int  testHoldMs{ 1500 };        // how long a test attack holds the button
		bool includeScripted{ false };  // also convert staves that carry scripts
		bool onlyListed{ true };        // development: convert only the staves in sInclude

		// "LocalFormID:Plugin.esp" entries, resolved after the plugins are known.
		std::vector<std::string> includeSpecs;
		std::vector<std::string> excludeSpecs;

		static Settings& Get();
		void             Load();
	};

	// Resolves "C00:SCSSoulCharge.esp" style entries to runtime FormIDs. Unknown entries are logged and skipped.
	[[nodiscard]] core::StaffFilterSettings ResolveFilterSettings(const Settings& a_settings);
}
