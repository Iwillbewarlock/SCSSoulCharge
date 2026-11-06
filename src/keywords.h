#pragma once

namespace scssc::keywords
{
	// Hooks TESObjectWEAP::InitItemImpl (vtable slot 0x13) at plugin load. Each staff that passes the
	// filter gets SpellCastingStaff_Keyword + one Basic tier keyword right after the engine initialises
	// it, i.e. before kDataLoaded, so SCS's one-time staff list (Controller.cpp:112-121) includes it.
	bool Install();

	struct Stats
	{
		std::uint32_t staves{ 0 };      // WEAPs with the staff animation seen by the hook
		std::uint32_t converted{ 0 };   // keywords added
		std::uint32_t skipped{ 0 };
		std::uint32_t scriptedSeen{ 0 };  // HasVMAD() true at InitItem time (T-VMAD evidence)
		bool          ready{ false };   // both plugins present and keywords resolved
		std::int64_t  lastAddMs{ 0 };   // steady clock, for T-ORD
		std::vector<RE::FormID> skippedBeforeReady;  // staves seen before setup succeeded (must stay empty)
	};

	[[nodiscard]] const Stats& GetStats();
	[[nodiscard]] bool         IsConverted(RE::FormID a_weapon);
	// HasVMAD() as seen when the staff was initialised; empty if that staff never reached the filter.
	[[nodiscard]] std::optional<bool> ScriptFlagAtInit(RE::FormID a_weapon);
	// The name the staff had before SCS could rename it (converted staves only).
	[[nodiscard]] const std::string* OriginalName(RE::FormID a_weapon);
}
