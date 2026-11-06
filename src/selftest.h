#pragma once

namespace scssc::selftest
{
	// One line per check: "SELFTEST PASS|FAIL <id> <detail>". Only when [SelfTest] bEnabled=1.
	void Report(std::string_view a_id, bool a_pass, std::string_view a_detail);

	// A sample that cannot be judged (item gone, cast cancelled): neither PASS nor FAIL.
	void Skip(std::string_view a_id, std::string_view a_detail);

	// Checks that need only loaded data (T-ORD, T-ESP). Called at kDataLoaded.
	void RunDataLoaded();
}
