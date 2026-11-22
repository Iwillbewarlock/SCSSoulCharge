// Offline tests for the game-independent rules. Exit code 0 = all passed.
#include "core/bindings.h"
#include "core/code_scan.h"
#include "core/spell_rules.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>

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

	void BindingTableTests()
	{
		using namespace scssc::core;
		const StaffKey a{ 0x4C6CA, 0x14, 3 };
		const StaffKey b{ 0x4C6CA, 0x14, 4 };  // same record, another item
		const NameState plain{ false, "Staff of Firebolts", "Staff of Firebolts [Ice Spike]" };

		BindingTable table;
		Check(table.Toggle(a, 0x2B96C, plain) == ToggleResult::kAssigned, "first pick assigns");
		Check(table.Find(b) == nullptr, "another item of the same record stays unbound");
		Check(table.Toggle(a, 0x12FD0, {}) == ToggleResult::kChanged && table.Find(a)->spell == 0x12FD0, "another spell replaces");
		Check(table.Find(a)->name.original == "Staff of Firebolts", "a change keeps the name state of the first pick");
		Check(table.Toggle(a, 0x12FD0, {}) == ToggleResult::kCleared && table.Find(a) == nullptr, "same spell again clears");

		// unique id moves
		(void)table.Toggle(a, 0x2B96C, plain);
		(void)table.Toggle(b, 0x12FD0, plain);
		Check(table.Move(a, b) && table.Size() == 1 && table.Find(b)->spell == 0x2B96C, "move replaces the binding at the new id");
		Check(!table.Move(a, b), "move from an unbound id reports false (failure)");
		Check(table.Move(b, b) && table.Size() == 1, "move onto itself keeps the binding (boundary)");

		// save round trip, with one form gone
		table.Clear();
		(void)table.Toggle(a, 0x2B96C, plain);
		(void)table.Toggle(b, 0x0DEAD, NameState{ true, "My staff", "My staff [Gone]" });
		const auto bytes = table.Serialize();
		BindingTable loaded;
		std::size_t  dropped = 9;
		std::size_t  orphaned = 9;
		const auto   resolve = [](FormID id) -> std::optional<FormID> {
			if (id == 0x0DEAD) {
				return std::nullopt;
			}
			return id == 0x4C6CA ? 0x0104C6CA : id;  // the staff moved in the load order
		};
		Check(loaded.Deserialize(bytes, resolve, &dropped, &orphaned), "round trip loads");
		Check(loaded.Size() == 2 && dropped == 0 && orphaned == 1, "binding with a missing spell is kept as an orphan and counted");
		const auto* orphan = loaded.Find(StaffKey{ 0x0104C6CA, 0x14, 4 });
		Check(orphan && orphan->spell == 0 && orphan->name.written == "My staff [Gone]", "orphan keeps the name state for restoring");
		Check(orphan && DecideRestore(orphan->name, "My staff [Gone]") == NameRestore::kRestoreOriginal, "orphan suffix can still be taken off");
		{
			BindingTable gone;
			const auto   noStaff = [](FormID id) -> std::optional<FormID> {
				return id == 0x4C6CA ? std::nullopt : std::optional<FormID>{ id };
			};
			Check(gone.Deserialize(bytes, noStaff, &dropped) && gone.Size() == 0 && dropped == 2, "binding with a missing staff is dropped");
		}
		const auto* kept = loaded.Find(StaffKey{ 0x0104C6CA, 0x14, 3 });
		Check(kept && kept->spell == 0x2B96C && kept->name.written == plain.written && !kept->name.hadOverride, "resolved staff key and name state survive");

		// malformed data
		auto truncated = bytes;
		truncated.pop_back();
		Check(!loaded.Deserialize(truncated, resolve) && loaded.Size() == 0, "truncated data is refused and leaves the table empty");
		auto trailing = bytes;
		trailing.push_back(0);
		Check(!loaded.Deserialize(trailing, resolve), "trailing bytes are refused");
		auto badVersion = bytes;
		badVersion[0] = 9;
		Check(!loaded.Deserialize(badVersion, resolve), "unknown format version is refused");
		std::vector<std::uint8_t> hugeName = { 1, 0, 0, 0, 1, 0, 0, 0 };
		for (int i = 0; i < 18; ++i) {
			hugeName.push_back(0);  // staff, uidBase, uid, spell, flags
		}
		hugeName.push_back(0xFF);
		hugeName.push_back(0xFF);  // name length 65535 > limit
		Check(!loaded.Deserialize(hugeName, resolve), "over-long saved name is refused");
		Check(loaded.Deserialize(std::vector<std::uint8_t>{ 1, 0, 0, 0, 0, 0, 0, 0 }, resolve) && loaded.Size() == 0, "empty table loads (boundary)");

		// names
		Check(ComposeName("Staff of Firebolts", "Ice Spike") == "Staff of Firebolts [Ice Spike]", "name gets the spell in brackets");
		Check(SpellNameForStaff("Ice Spike (Learning - 35%)") == "Ice Spike", "a progress note is left out of the staff name");
		Check(SpellNameForStaff("Ice Spike") == "Ice Spike", "a plain name stays (boundary)");
		Check(SpellNameForStaff("Conjure (Familiar)") == "Conjure (Familiar)", "parentheses without a percentage stay");
		Check(SpellNameForStaff("(50%)") == "(50%)", "a name that is only the note stays (failure guard)");
		Check(DecideRestore(plain, plain.written) == NameRestore::kRemoveOverride, "our name on an item without its own name is removed");
		Check(DecideRestore(NameState{ true, "My staff", "My staff [X]" }, "My staff [X]") == NameRestore::kRestoreOriginal, "own name comes back");
		Check(DecideRestore(plain, "Renamed by player") == NameRestore::kLeave, "a later rename is kept");
		Check(DecideRestore(NameState{ false, "Staff", "" }, "Staff") == NameRestore::kLeave, "nothing written, nothing restored (boundary)");
		const auto rebased = Rebase(plain, "Renamed by player");
		Check(rebased.hadOverride && rebased.original == "Renamed by player" && rebased.written.empty(), "a later rename becomes the new original");
		Check(Rebase(plain, plain.written).original == plain.original, "our own name keeps the original");
	}
	void SpellRuleTests()
	{
		using namespace scssc::core;
		SpellTraits firebolt;
		firebolt.isPlainSpell = true;
		firebolt.known = true;
		firebolt.fireAndForget = true;
		firebolt.hasEffects = true;
		Check(ClassifyForStaff(firebolt, false) == BindVerdict::kAllowed, "a known fire-and-forget spell is allowed");
		auto flames = firebolt;
		flames.fireAndForget = false;
		flames.concentration = true;
		Check(ClassifyForStaff(flames, false) == BindVerdict::kAllowed, "a concentration spell is allowed");
		auto summon = firebolt;
		summon.range = 0.01f;
		Check(ClassifyForStaff(summon, false) == BindVerdict::kRange, "any range above zero is refused (boundary)");
		auto oakflesh = firebolt;
		oakflesh.selfDelivery = true;
		oakflesh.noAbsorb = true;
		Check(ClassifyForStaff(oakflesh, false) == BindVerdict::kAllowed, "no-absorb on a self spell is allowed");
		auto drain = firebolt;
		drain.noAbsorb = true;
		Check(ClassifyForStaff(drain, false) == BindVerdict::kNoAbsorb, "no-absorb on an aimed spell is refused");
		auto power = firebolt;
		power.isPlainSpell = false;
		Check(ClassifyForStaff(power, false) == BindVerdict::kNotASpell, "powers are refused");
		auto unknown = firebolt;
		unknown.known = false;
		Check(ClassifyForStaff(unknown, false) == BindVerdict::kNotKnown, "an unknown spell is refused");
		auto ability = firebolt;
		ability.fireAndForget = false;
		Check(ClassifyForStaff(ability, false) == BindVerdict::kCastingType, "constant effect is refused");
		auto ignore = firebolt;
		ignore.ignoreResistance = true;
		Check(ClassifyForStaff(ignore, false) == BindVerdict::kIgnoreResistance, "ignore-resistance is refused");
		Check(std::string_view(Describe(BindVerdict::kRange)).size() > 0, "every verdict has a description");
		Check(ClassifyForStaff(summon, true) == BindVerdict::kAllowed, "with the copy keeping spell values a ranged spell is allowed");
		Check(ClassifyForStaff(ignore, true) == BindVerdict::kAllowed && ClassifyForStaff(drain, true) == BindVerdict::kAllowed,
			"with the copy keeping spell values the flags no longer rule a spell out");
		Check(ClassifyForStaff(power, true) == BindVerdict::kNotASpell && ClassifyForStaff(unknown, true) == BindVerdict::kNotKnown &&
				  ClassifyForStaff(ability, true) == BindVerdict::kCastingType,
			"spell type, knowledge and casting type still rule a spell out (failure guard)");
		auto runtime = firebolt;
		runtime.runtimeForm = true;
		Check(ClassifyForStaff(runtime, true) == BindVerdict::kRuntimeForm, "a spell made during play is refused");
	}

	// Final review 2026-11-19: dormant vs gone, the release signals, the marker's owner, the binding cap.
	void FinalReviewRuleTests()
	{
		using namespace scssc::core;
		Check(DecideHeld(false, 0x2B96C, true, false, true) == HeldBinding::kNone, "no binding, nothing to do");
		Check(DecideHeld(true, 0x2B96C, true, false, true) == HeldBinding::kActive, "a known spell is active");
		Check(DecideHeld(true, 0x2B96C, true, false, false) == HeldBinding::kDormant, "an unknown spell that exists waits");
		Check(DecideHeld(true, 0x2B96C, false, false, false) == HeldBinding::kUnbind, "a spell id that finds no form ends the binding");
		Check(DecideHeld(true, 0, false, false, false) == HeldBinding::kUnbind, "an orphan (spell plugin gone) ends the binding");
		Check(DecideHeld(true, 0xFF00012Au, true, true, true) == HeldBinding::kUnbind, "a runtime spell from before the load ends the binding");
		Check(DecideHeld(true, 0xFF00012Au, true, false, true) == HeldBinding::kUnbind, "a 0xFF id ends the binding even if the form is not flagged (boundary)");

		Check(CastReleased(ReleaseSignals{ .releaseState = true }), "a release state counts");
		Check(CastReleased(ReleaseSignals{ .effectApplied = true, .otherHandCast = true }), "an applied effect counts even with the left hand busy");
		Check(CastReleased(ReleaseSignals{ .paidEnough = true }), "magicka paid counts when nothing else drained it");
		Check(!CastReleased(ReleaseSignals{ .paidEnough = true, .otherHandCast = true }), "a drop while the left hand cast does not count (failure)");
		Check(!CastReleased(ReleaseSignals{ .paidEnough = true, .slotChanged = true }), "a drop after the slot changed does not count (failure)");
		Check(!CastReleased(ReleaseSignals{}), "no signal, no message (boundary)");

		Check(WithoutMarker("Firebolt \xE2\x98\x85", " \xE2\x98\x85", true) == "Firebolt", "our marker comes off");
		Check(WithoutMarker("Firebolt II", " II", false) == "Firebolt II", "a name that ends like the marker keeps it when we did not add it");
		Check(WithoutMarker("Firebolt", " \xE2\x98\x85", true) == "Firebolt", "no marker, name unchanged (boundary)");
		Check(WithoutMarker("Firebolt \xE2\x98\x85", "", true) == "Firebolt \xE2\x98\x85", "marker off, name unchanged (boundary)");

		BindingTable full;
		const std::string longName(BindingTable::kMaxNameBytes + 40, 'x');
		for (std::uint32_t i = 0; i < BindingTable::kMaxBindings; ++i) {
			const StaffKey key{ 0x4C6CA, 0x14 + i, static_cast<std::uint16_t>(i) };
			(void)full.Toggle(key, 0x2B96C, NameState{ true, longName, longName });
		}
		Check(full.Full() && full.Size() == BindingTable::kMaxBindings, "the table reports full at the cap");
		Check(full.Serialize().size() <= BindingTable::kMaxSerializedBytes, "a full table with the longest names fits the reader's limit");
		Check(BindingTable::kMaxSerializedBytes <= (4u << 20), "the limit stays under the 4 MiB the old reader accepted");
	}

	// Start-up address checks (runtime_check.cpp) on hand-made code. Code "lives" at 0x1000.
	void CodeScanTests()
	{
		using namespace scssc::core;
		constexpr std::uintptr_t kAt = 0x1000;
		// +0: call 0x1100 (rel 0xFB), +5: mov eax,[rcx+0x80], +11: jmp 0x1200 (rel 0x1F0 from 0x1010), +16: call 0x1100 again
		const std::vector<std::uint8_t> code{
			0xE8, 0xFB, 0x00, 0x00, 0x00,              // call 0x1100
			0x8B, 0x81, 0x80, 0x00, 0x00, 0x00,        // mov eax, [rcx+0x80]
			0xE9, 0xF0, 0x01, 0x00, 0x00,              // jmp 0x1200
			0xE8, 0xEB, 0x00, 0x00, 0x00,              // call 0x1100 (next instruction at 0x1015)
		};
		const std::vector<std::uintptr_t> callee{ 0x1100 };
		const std::vector<std::uintptr_t> tail{ 0x1200 };
		const std::vector<std::uintptr_t> both{ 0x1100, 0x1200, 0x1300 };

		Check(Rel32Target(code, kAt, 0) == 0x1100, "rel32 destination of a call");
		Check(Rel32Target(code, kAt, 11) == 0x1200, "rel32 destination of a jump");
		Check(FindBranchesTo(code, kAt, callee, false).size() == 2, "both calls to the expected function are found");
		Check(CountCalledTargets(code, kAt, both) == 1, "a function called twice counts once; a jump is not a call");
		Check(FindBranchesTo(code, kAt, tail, false).empty(), "a jump is not found when only calls are asked for (failure)");
		const auto jumps = FindBranchesTo(code, kAt, tail, true);
		Check(jumps.size() == 1 && jumps[0].offset == 11 && jumps[0].kind == Branch::kJump, "the tail jump is found when jumps count");
		Check(HasDisp32(code, 0x80) && !HasDisp32(code, 0x88), "member displacement +0x80 present, +0x88 absent");
		const std::vector<std::uint8_t> movModrm{ 0x8B, 0x81 };
		const std::vector<std::uint8_t> reversed{ 0x81, 0x8B };
		Check(ContainsBytes(code, movModrm) && !ContainsBytes(code, reversed), "a byte run is found only in its own order");
		Check(!ContainsBytes(code, std::vector<std::uint8_t>{}), "an empty byte run never matches (boundary)");

		// lock xadd, with and without REX; a truncated one at the buffer end does not count
		const std::vector<std::uint8_t> xadd{ 0x90, 0xF0, 0x0F, 0xC1, 0x01 };        // lock xadd [rcx], eax
		const std::vector<std::uint8_t> xaddRex{ 0xF0, 0x44, 0x0F, 0xC1, 0x01 };     // lock xadd [rcx], r8d
		const std::vector<std::uint8_t> lockInc{ 0xF0, 0xFF, 0x81, 0x80, 0x00, 0x00, 0x00 };  // lock inc [rcx+0x80]
		const std::vector<std::uint8_t> cut{ 0x90, 0xF0, 0x44, 0x0F };               // ends before C1
		Check(HasLockXadd(xadd) && HasLockXadd(xaddRex), "lock xadd found with and without REX");
		Check(!HasLockXadd(lockInc), "lock inc is not a lock xadd (failure)");
		Check(!HasLockXadd(cut), "a lock xadd cut at the buffer end does not count (boundary)");

		// boundary: a branch in the last five bytes counts; four bytes are too few; an offset past the end gives 0
		const std::vector<std::uint8_t> tailOnly(code.end() - 5, code.end());
		Check(FindBranchesTo(tailOnly, kAt + 0x10, callee, false).size() == 1, "a call filling the whole buffer is found (boundary)");
		const std::vector<std::uint8_t> shortCode(code.begin(), code.begin() + 4);
		Check(FindBranchesTo(shortCode, kAt, callee, false).empty() && Rel32Target(shortCode, kAt, 0) == 0, "four bytes hold no rel32 branch (boundary)");
		Check(Rel32Target(code, kAt, code.size()) == 0 && Rel32Target(code, kAt, code.size() + 7) == 0, "an offset at or past the end gives 0 (failure)");

		// failure: the same bytes at another address reach other destinations, so nothing matches
		Check(CountCalledTargets(code, kAt + 0x40, both) == 0, "moved code does not match the expected destinations (failure)");
		// a backwards call (negative displacement)
		const std::vector<std::uint8_t> back{ 0xE8, 0xFB, 0xFF, 0xFF, 0xFF };  // call kAt + 5 - 5 = kAt
		Check(Rel32Target(back, kAt, 0) == kAt, "negative displacement");
	}
}

int main()
{
	BindingTableTests();
	SpellRuleTests();
	FinalReviewRuleTests();
	CodeScanTests();
	std::printf("core_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
