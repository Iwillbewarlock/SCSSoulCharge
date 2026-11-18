#include "PCH.h"

#include "selftest.h"

#include "settings.h"

namespace scssc::selftest
{
	void Report(std::string_view a_id, bool a_pass, std::string_view a_detail)
	{
		if (!Settings::Get().selfTest) {
			return;
		}
		if (a_pass) {
			logger::info("SELFTEST PASS {} {}", a_id, a_detail);
		} else {
			logger::error("SELFTEST FAIL {} {}", a_id, a_detail);
		}
	}

	void Skip(std::string_view a_id, std::string_view a_detail)
	{
		if (Settings::Get().selfTest) {
			logger::info("SELFTEST SKIP {} {}", a_id, a_detail);
		}
	}
}
