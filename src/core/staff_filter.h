#pragma once

#include <cstdint>
#include <string_view>
#include <unordered_set>

// Which staff WEAP records get the SCS keywords.

namespace scssc::core
{
	struct StaffFacts
	{
		std::uint32_t formID{ 0 };
		bool          isStaffAnimation{ false };
		bool          hasEnchantment{ false };
		float         enchantmentCost{ 0.0f };  // record cost; auto-calc enchantments report their computed value
		bool          hasScsKeyword{ false };
		bool          hasScript{ false };
	};

	struct StaffFilterSettings
	{
		bool                              includeScripted{ false };
		bool                              onlyListed{ false };  // test mode: only formIDs in `include`
		std::unordered_set<std::uint32_t> include;               // overrides the script exclusion
		std::unordered_set<std::uint32_t> exclude;
	};

	enum class StaffVerdict
	{
		kEligible,
		kNotStaff,
		kNoEnchantment,
		kZeroCost,
		kAlreadyScs,
		kExcluded,
		kScripted,
		kNotListed,
	};

	[[nodiscard]] StaffVerdict      ClassifyStaff(const StaffFacts& a_staff, const StaffFilterSettings& a_settings);
	[[nodiscard]] std::string_view  VerdictName(StaffVerdict a_verdict);
}
