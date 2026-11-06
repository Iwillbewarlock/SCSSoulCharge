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
			std::string   foreign;  // keyworded by neither SpellCastingStaff.esp nor us (another mod or patch)
			if (data && keyword) {
				for (auto* weapon : data->GetFormArray<RE::TESObjectWEAP>()) {
					if (!weapon || !weapon->HasKeyword(keyword)) {
						continue;
					}
					++withKeyword;
					const auto* file = weapon->GetFile(0);
					const bool  scsOwn = file && file->GetFilename() == kScsPlugin;
					if (!scsOwn && !keywords::IsConverted(weapon->GetFormID())) {
						foreign += std::format(" {:08X}", weapon->GetFormID());
					}
				}
			}
			const auto filter = ResolveFilterSettings(Settings::Get());
			// Listed staves the filter should convert (exclude wins over include, staff_filter.cpp).
			std::uint32_t listedExpected = 0;
			std::uint32_t listedMissing = 0;
			for (const auto id : filter.include) {
				if (filter.exclude.contains(id)) {
					continue;
				}
				++listedExpected;
				if (!keywords::IsConverted(id)) {
					++listedMissing;
				}
			}
			const bool somethingExpected = !filter.onlyListed || listedExpected > 0;
			const bool pass = stats.ready && stats.skippedBeforeReady.empty() && (!somethingExpected || stats.converted > 0) && listedMissing == 0 &&
			                  withKeyword == kScsOwnStaves + stats.converted + static_cast<std::uint32_t>(std::ranges::count(foreign, ' '));
			Report("T-ORD", pass,
				std::format("staves={} converted={} skipped={} skippedBeforeReady={} listedNotConverted={} weaponsWithScsKeyword={} "
							"foreign=[{} ] (SpellCastingStaff.log with bVerboseLog=1 must say 'ready: {} keyword staffs')",
					stats.staves, stats.converted, stats.skipped, stats.skippedBeforeReady.size(), listedMissing, withKeyword, foreign,
					withKeyword));
		}

		// Is HasVMAD usable at InitItem time? Known scripted staff vs known plain staff (hypothesis check).
		void CheckScriptDetection()
		{
			auto* data = RE::TESDataHandler::GetSingleton();
			auto* scripted = data ? data->LookupForm<RE::TESObjectWEAP>(0xD62, "ccbgssse019-staffofsheogorath.esl") : nullptr;
			auto* plain = data ? data->LookupForm<RE::TESObjectWEAP>(0x4C6CA, "Skyrim.esm") : nullptr;
			if (!scripted || !plain) {
				Report("T-VMAD", false, std::format("reference staves missing (Sheogorath={} Firebolts={}); not judged", scripted != nullptr, plain != nullptr));
				return;
			}
			// The filter runs at InitItem time, so that is the value that matters.
			const auto scriptedInit = keywords::ScriptFlagAtInit(scripted->GetFormID());
			const auto plainInit = keywords::ScriptFlagAtInit(plain->GetFormID());
			const bool judged = scriptedInit.has_value() && plainInit.has_value();
			Report("T-VMAD", judged && *scriptedInit && !*plainInit,
				std::format("at InitItem: StaffOfSheogorath={} (expect 1) StaffOfFirebolts={} (expect 0); at kDataLoaded: {} / {}{}",
					scriptedInit ? (*scriptedInit ? "1" : "0") : "not seen", plainInit ? (*plainInit ? "1" : "0") : "not seen",
					scripted->HasVMAD(), plain->HasVMAD(), judged ? "" : " (InitItem value missing: not judged)"));
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
		CheckScriptDetection();
	}
}
