// Offline tests for the game-independent rules. Exit code 0 = all passed.
#include "core/charge_rules.h"
#include "core/pool_table.h"
#include "core/staff_filter.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <map>

namespace
{
	int g_failures = 0;
	int g_checks = 0;

	void Check(bool a_ok, const char* a_what)
	{
		++g_checks;
		if (!a_ok) {
			++g_failures;
			std::printf("FAIL: %s\n", a_what);
		}
	}

	bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

	void ChargeRules()
	{
		using namespace scssc::core;
		const float nan = std::numeric_limits<float>::quiet_NaN();
		// fire-and-forget
		Check(AllowFireAndForget(50.0f, 36.5f), "F&F allowed with enough charge");
		Check(AllowFireAndForget(36.5f, 36.5f), "F&F allowed at exactly the cost (boundary)");
		Check(!AllowFireAndForget(36.0f, 36.5f), "F&F refused below the cost");
		Check(!AllowFireAndForget(nan, 1.0f) && !AllowFireAndForget(10.0f, -1.0f), "F&F refused on invalid numbers");
		Check(Near(FireAndForgetDebit(36.5f, false), 36.5f), "F&F debit is the cost");
		Check(FireAndForgetDebit(36.5f, true) == 0.0f, "failed target-location cast costs nothing");
		// concentration start
		Check(AllowConcentrationStart(5.0f, 20.0f), "concentration starts with a little charge (per-second cost)");
		Check(!AllowConcentrationStart(0.0f, 20.0f), "concentration refused at zero charge");
		Check(AllowConcentrationStart(0.0f, 0.0f), "free concentration always starts");
		// concentration tick
		auto t = ConcentrationTick(100.0f, 20.0f, 0.1f);
		Check(Near(t.debit, 2.0f) && !t.interrupt, "tick bills cost x dt");
		t = ConcentrationTick(1.0f, 20.0f, 0.1f);
		Check(Near(t.debit, 1.0f) && t.interrupt, "tick never bills more than what is left, then stops");
		t = ConcentrationTick(100.0f, 20.0f, 0.3f);
		Check(t.debit == 0.0f && t.interrupt, "large frame gap stops without billing");
		t = ConcentrationTick(100.0f, 20.0f, nan);
		Check(t.debit == 0.0f && t.interrupt, "invalid dt stops without billing");
	}

	void PoolTableTests()
	{
		using namespace scssc::core;
		PoolTable table(3);
		Check(table.Assign(0x100) == 0u, "first enchantment gets slot 0");
		Check(table.Assign(0x200) == 1u, "second enchantment gets slot 1");
		Check(table.Assign(0x100) == 0u, "same enchantment keeps its slot");
		Check(table.Assign(0x300) == 2u, "third fills the pool");
		Check(!table.Assign(0x400).has_value(), "full pool refuses a new enchantment (no reuse)");
		Check(!table.Assign(0).has_value(), "FormID 0 is never assigned");

		// round trip with a changed load order and one removed enchantment
		const auto bytes = table.Serialize();
		const std::map<FormID, FormID> moved{ { 0x100, 0x1100 }, { 0x300, 0x3300 } };  // 0x200 is gone
		PoolTable loaded(3);
		const bool ok = loaded.Deserialize(bytes, [&](FormID a_id) -> std::optional<FormID> {
			const auto it = moved.find(a_id);
			return it == moved.end() ? std::nullopt : std::optional<FormID>(it->second);
		});
		Check(ok, "round trip succeeds");
		Check(loaded.Find(0x1100) == 0u && loaded.Find(0x3300) == 2u, "resolved FormIDs keep their slots");
		Check(!loaded.EnchantmentAt(1).has_value() && loaded.Used() == 3, "vanished enchantment keeps its slot reserved");
		Check(!loaded.Assign(0x500).has_value(), "reserved slot is not reused");

		// malformed data
		auto bad = bytes;
		bad.pop_back();
		Check(!loaded.Deserialize(bad, [](FormID a) { return std::optional<FormID>(a); }) && loaded.Used() == 0,
			"truncated data is refused and leaves the table empty");
		auto dup = bytes;
		dup[12] = 0;  // first entry's slot (bytes 12..15) -> 0, clashing with nothing; make second entry slot 0 too
		dup[20] = 0;
		Check(!loaded.Deserialize(dup, [](FormID a) { return std::optional<FormID>(a); }), "duplicate slot numbers are refused");
		PoolTable small(2);
		Check(!small.Deserialize(bytes, [](FormID a) { return std::optional<FormID>(a); }), "more entries than capacity are refused");
		auto twice = bytes;
		twice[16] = twice[8];  // second entry's enchantment = first entry's
		twice[17] = twice[9];
		twice[18] = twice[10];
		twice[19] = twice[11];
		Check(!loaded.Deserialize(twice, [](FormID a) { return std::optional<FormID>(a); }), "one enchantment in two slots is refused");
	}

	void StaffFilterTests()
	{
		using namespace scssc::core;
		StaffFilterSettings settings;
		StaffFacts          staff{ 0xABC, true, true, 60.0f, false, false };
		Check(ClassifyStaff(staff, settings) == StaffVerdict::kEligible, "ordinary enchanted staff is eligible");

		auto s = staff;
		s.isStaffAnimation = false;
		Check(ClassifyStaff(s, settings) == StaffVerdict::kNotStaff, "non-staff weapon is skipped");
		s = staff;
		s.hasEnchantment = false;
		Check(ClassifyStaff(s, settings) == StaffVerdict::kNoEnchantment, "staff without enchantment is skipped");
		s = staff;
		s.enchantmentCost = 0.0f;
		Check(ClassifyStaff(s, settings) == StaffVerdict::kZeroCost, "zero-cost enchantment is skipped (boundary)");
		s = staff;
		s.hasScsKeyword = true;
		Check(ClassifyStaff(s, settings) == StaffVerdict::kAlreadyScs, "SCS staff is never touched");
		s = staff;
		s.hasScript = true;
		Check(ClassifyStaff(s, settings) == StaffVerdict::kScripted, "scripted staff is excluded by default");
		settings.include.insert(0xABC);
		Check(ClassifyStaff(s, settings) == StaffVerdict::kEligible, "include list overrides the script exclusion");
		settings.exclude.insert(0xABC);
		Check(ClassifyStaff(s, settings) == StaffVerdict::kExcluded, "exclude list wins over include list");
		StaffFilterSettings testOnly;
		testOnly.onlyListed = true;
		Check(ClassifyStaff(staff, testOnly) == StaffVerdict::kNotListed, "test mode skips unlisted staves");
		s = staff;
		s.hasScsKeyword = true;
		testOnly.include.insert(0xABC);
		Check(ClassifyStaff(s, testOnly) == StaffVerdict::kAlreadyScs, "include list cannot convert an SCS staff");
	}
}

int main()
{
	ChargeRules();
	PoolTableTests();
	StaffFilterTests();
	std::printf("core_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
