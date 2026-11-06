#include "PCH.h"

#include "selftest.h"

#include "keywords.h"
#include "settings.h"

namespace scssc::selftest
{
	namespace
	{
		constexpr auto       kOwnPlugin = "SCSSoulCharge.esp"sv;
		constexpr auto       kScsPlugin = "SpellCastingStaff.esp"sv;
		constexpr RE::FormID kFirstPoolSpell = 0x800;
		constexpr RE::FormID kPoolSize = 1024;
		constexpr RE::FormID kTestStaff = 0xC00;
		constexpr RE::FormID kScsStaffKeyword = 0x803;
		constexpr std::uint32_t kScsOwnStaves = 27;  // WEAPs SpellCastingStaff.esp 2.1.1 defines

		void CheckOrder()
		{
			const auto& stats = keywords::GetStats();
			auto*       data = RE::TESDataHandler::GetSingleton();
			auto*       keyword = data ? data->LookupForm<RE::BGSKeyword>(kScsStaffKeyword, kScsPlugin) : nullptr;
			std::uint32_t withKeyword = 0;
			if (data && keyword) {
				for (auto* weapon : data->GetFormArray<RE::TESObjectWEAP>()) {
					if (weapon && weapon->HasKeyword(keyword)) {
						++withKeyword;
					}
				}
			}
			const bool pass = stats.ready && withKeyword == kScsOwnStaves + stats.converted;
			Report("T-ORD", pass,
				std::format("staves={} converted={} skipped={} weaponsWithScsKeyword={} expected={} "
							"(compare with SpellCastingStaff.log 'N keyword staffs': N must equal weaponsWithScsKeyword)",
					stats.staves, stats.converted, stats.skipped, withKeyword, kScsOwnStaves + stats.converted));
		}

		void CheckEsp()
		{
			auto*         data = RE::TESDataHandler::GetSingleton();
			std::uint32_t found = 0;
			std::uint32_t empty = 0;
			for (RE::FormID i = 0; data && i < kPoolSize; ++i) {
				if (auto* spell = data->LookupForm<RE::SpellItem>(kFirstPoolSpell + i, kOwnPlugin)) {
					++found;
					if (spell->effects.empty()) {
						++empty;
					}
				}
			}
			auto* staff = data ? data->LookupForm<RE::TESObjectWEAP>(kTestStaff, kOwnPlugin) : nullptr;
			Report("T-ESP", found == kPoolSize && empty == kPoolSize && staff,
				std::format("poolSpells={}/{} withoutEffects={} testStaff={}", found, kPoolSize, empty, staff != nullptr));
		}
	}

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

	void RunDataLoaded()
	{
		CheckOrder();
		CheckEsp();
	}
}
