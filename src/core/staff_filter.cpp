#include "core/staff_filter.h"

#include <cmath>

namespace scssc::core
{
	StaffVerdict ClassifyStaff(const StaffFacts& a_staff, const StaffFilterSettings& a_settings)
	{
		if (!a_staff.isStaffAnimation) {
			return StaffVerdict::kNotStaff;
		}
		if (!a_staff.hasEnchantment) {
			return StaffVerdict::kNoEnchantment;
		}
		if (a_staff.hasScsKeyword) {
			return StaffVerdict::kAlreadyScs;
		}
		if (!std::isfinite(a_staff.enchantmentCost) || a_staff.enchantmentCost <= 0.0f) {
			return StaffVerdict::kZeroCost;
		}
		if (a_settings.exclude.contains(a_staff.formID)) {
			return StaffVerdict::kExcluded;
		}
		const bool listed = a_settings.include.contains(a_staff.formID);
		if (a_settings.onlyListed && !listed) {
			return StaffVerdict::kNotListed;
		}
		if (a_staff.hasScript && !a_settings.includeScripted && !listed) {
			return StaffVerdict::kScripted;
		}
		return StaffVerdict::kEligible;
	}

	std::string_view VerdictName(StaffVerdict a_verdict)
	{
		switch (a_verdict) {
		case StaffVerdict::kEligible:
			return "eligible";
		case StaffVerdict::kNotStaff:
			return "not a staff";
		case StaffVerdict::kNoEnchantment:
			return "no enchantment";
		case StaffVerdict::kZeroCost:
			return "enchantment cost 0";
		case StaffVerdict::kAlreadyScs:
			return "already an SCS staff";
		case StaffVerdict::kExcluded:
			return "excluded by INI";
		case StaffVerdict::kScripted:
			return "has scripts (excluded by default)";
		case StaffVerdict::kNotListed:
			return "not in the test include list";
		}
		return "unknown";
	}
}
