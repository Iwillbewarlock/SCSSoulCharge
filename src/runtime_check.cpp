#include "runtime_check.h"

#include "core/code_scan.h"
#include "selftest.h"

#include <array>
#include <format>
#include <span>

#include <Windows.h>

namespace scssc::runtime_check
{
	namespace
	{
		struct Ids
		{
			std::uint64_t se;
			std::uint64_t ae;
		};

		// One engine function: its id pair and its body size per build (distance to the next address-library id:
		// the same in every AE library 1.6.317-1.7.104; the SE size from the 1.5.97 library). The scan never reads
		// past the function on a known build. tools/runtime_check_harness.py reads these tables, keep the layout.
		struct Function
		{
			const char* name;  // T-RUNTIME-<name>
			Ids         id;
			std::size_t seSize;
			std::size_t aeSize;
		};

		constexpr Function kPreload{ "PRELOAD", { 11195, 11303 }, 0x1C0, 0x230 };
		constexpr Function kRelease{ "RELEASE", { 11198, 11306 }, 0x90, 0x90 };
		constexpr Function kSetArt{ "SETART", { 33369, 34150 }, 0x30, 0x30 };
		constexpr Function kDetach{ "DETACH", { 33371, 34152 }, 0xD0, 0x230 };
		constexpr Function kChargeCaller{ "CHARGE", { 37868, 38823 }, 0xF0, 0xF0 };

		// Callees each function makes in the 1.7.104 executable (AE ids) and their SE matches (meh321's list).
		// 11303 also calls 68121, which has no SE match. 34152's 77875 is left out: its neighbour 34151 calls it too.
		constexpr std::array<Ids, 6> kPreloadCallees{ { { 11199, 11307 }, { 11194, 11302 }, { 66859, 68115 }, { 11227, 11346 }, { 13996, 14087 }, { 73999, 75738 } } };
		constexpr std::size_t        kPreloadNeeded = 3;
		constexpr Ids                kReleaseCallee{ 73900, 75637 };
		constexpr Ids                kSetArtTail{ 33373, 34154 };  // 34150 ends with a JMP there
		constexpr std::array<Ids, 3> kDetachCallees{ { { 14861, 15030 }, { 32153, 32897 }, { 73901, 75638 } } };
		constexpr std::size_t        kDetachNeeded = 2;
		// 38822, right before 38823, also calls the resource lookup once; only 38823 calls these two.
		constexpr std::array<Ids, 2> kChargeCallees{ { { 14411, 14563 }, { 14412, 14564 } } };
		constexpr std::size_t        kChargeNeeded = 1;

		// Main::Update and the two per-frame functions it calls that plugins hook: 21873 (this mod on 1.6.1170 at
		// +0xC26, SCS 2.1.1 too; 1.7.104 +0xC38) and 78641 (most public plugins: +0xC26 up to 1.6.659, +0xC2B from
		// 1.6.1130; 1.7.104 +0xC3D). SE: +0x748 (SCS and public plugins).
		constexpr Ids                kMainUpdate{ 35565, 36564 };
		constexpr std::array<Ids, 2> kFrameTargets{ { { 21400, 21873 }, { 76765, 78641 } } };
		constexpr Ids                kMenuEquipTarget{ 37973, 38928 };


		// The virtual-function slots this mod overwrites, and the function each must hold before the write (read in the
		// 1.6.1170 and 1.7.104 executables; SE ids from meh321's list, 1.0 for the EnchantmentItem slots, 0.758 for
		// CheckCast). MagicTarget::AddTarget (38786) has no SE match: on SE the Character and PlayerCharacter slots must
		// hold the same function, at a function start. copy_traits.cpp, resource_hook.cpp, effect_redirect.cpp.
		enum class Table
		{
			kEnchantmentItem,
			kActorMagicCaster,
			kCharacterMagicTarget,
			kPlayerMagicTarget,
		};

		struct VSlot
		{
			Table       table;
			std::size_t slot;
			Ids         expected;  // se 0 = no SE match
		};

		constexpr std::array<VSlot, 10> kVSlots{ {
			{ Table::kEnchantmentItem, 0x59, { 11177, 11284 } },
			{ Table::kEnchantmentItem, 0x5A, { 11178, 11285 } },
			{ Table::kEnchantmentItem, 0x5B, { 11184, 11291 } },
			{ Table::kEnchantmentItem, 0x5C, { 11185, 11292 } },
			{ Table::kEnchantmentItem, 0x5E, { 11187, 11294 } },
			{ Table::kEnchantmentItem, 0x5F, { 11188, 11295 } },
			{ Table::kEnchantmentItem, 0x60, { 11200, 11308 } },
			{ Table::kActorMagicCaster, 0x0A, { 33364, 34145 } },
			{ Table::kCharacterMagicTarget, 0x01, { 0, 38786 } },
			{ Table::kPlayerMagicTarget, 0x01, { 0, 38786 } },
		} };

		bool IsSE()
		{
			return REL::Module::GetRuntime() == REL::Module::Runtime::SE;
		}

		std::uint64_t IdOf(const Ids& a_ids)
		{
			return IsSE() ? a_ids.se : a_ids.ae;
		}

		std::uintptr_t Address(const Ids& a_ids)
		{
			return REL::Relocation<std::uintptr_t>{ REL::RelocationID(a_ids.se, a_ids.ae) }.address();
		}

		std::span<const std::uint8_t> Code(std::uintptr_t a_address, std::size_t a_size)
		{
			return { reinterpret_cast<const std::uint8_t*>(a_address), a_size };
		}

		std::size_t SizeOf(const Function& a_fn)
		{
			return IsSE() ? a_fn.seSize : a_fn.aeSize;
		}

		std::uintptr_t GameOffset(std::uintptr_t a_address)
		{
			return a_address - REL::Module::get().base();
		}

		std::string Build()
		{
			return std::format("{} {}", IsSE() ? "SE" : "AE", REL::Module::get().version().string("."sv));
		}

		// Always logged (the log is how a player on a build nobody here runs tells us what happened), and a SELFTEST
		// line too when [SelfTest] bEnabled=1.
		void Report(std::string_view a_id, bool a_pass, std::string_view a_detail)
		{
			if (a_pass) {
				logger::info("T-RUNTIME PASS {} ({}): {}", a_id, Build(), a_detail);
			} else {
				logger::critical("T-RUNTIME FAIL {} ({}): {}", a_id, Build(), a_detail);
			}
			selftest::Report(std::format("T-RUNTIME-{}", a_id), a_pass, a_detail);
		}

		template <std::size_t N>
		std::array<std::uintptr_t, N> Addresses(const std::array<Ids, N>& a_ids)
		{
			std::array<std::uintptr_t, N> out{};
			for (std::size_t i = 0; i < N; ++i) {
				out[i] = Address(a_ids[i]);
			}
			return out;
		}

		// The game's own unwind table (.pdata) names every function that has a stack frame. An address with an entry
		// must be where that entry begins; one without an entry (a leaf function such as 34150) is let through.
		enum class Start
		{
			kYes,
			kNoEntry,
			kMidFunction,
		};

		Start FunctionStart(std::uintptr_t a_address)
		{
			DWORD64     imageBase = 0;
			const auto* entry = ::RtlLookupFunctionEntry(static_cast<DWORD64>(a_address), &imageBase, nullptr);
			if (!entry) {
				return Start::kNoEntry;
			}
			return imageBase + entry->BeginAddress == a_address ? Start::kYes : Start::kMidFunction;
		}

		std::string_view Describe(Start a_start)
		{
			switch (a_start) {
			case Start::kYes:
				return "a function start";
			case Start::kNoEntry:
				return "no unwind entry (leaf)";
			default:
				return "INSIDE another function";
			}
		}

		// Committed, executable memory: a hooked call's destination outside SkyrimSE.exe must at least be code.
		bool IsExecutable(std::uintptr_t a_address)
		{
			MEMORY_BASIC_INFORMATION info{};
			if (::VirtualQuery(reinterpret_cast<LPCVOID>(a_address), &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT) {
				return false;
			}
			constexpr DWORD kExecute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			return (info.Protect & kExecute) != 0 && (info.Protect & PAGE_GUARD) == 0;
		}

		// A CALL's destination is acceptable when it is one of a_known, or code outside SkyrimSE.exe (another plugin
		// hooked the same call first). a_why gets the reason either way.
		bool DestinationOk(std::uintptr_t a_target, std::span<const std::uintptr_t> a_known, std::span<const Ids> a_knownIds, std::string& a_why)
		{
			if (const auto it = std::ranges::find(a_known, a_target); it != a_known.end()) {
				a_why = std::format("known function {}", IdOf(a_knownIds[static_cast<std::size_t>(it - a_known.begin())]));
				return true;
			}
			if (!InGameModule(a_target)) {
				const bool code = IsExecutable(a_target);
				a_why = code ? "code outside SkyrimSE.exe: another plugin hooked it" : "outside SkyrimSE.exe and not executable memory";
				return code;
			}
			a_why = std::format("game +{:X}, not an expected function", GameOffset(a_target));
			return false;
		}

		// A function checked by the callees it must call (at least a_needed different ones).
		template <std::size_t N>
		std::uintptr_t CheckByCallees(const Function& a_fn, const std::array<Ids, N>& a_callees, std::size_t a_needed)
		{
			const auto address = Address(a_fn.id);
			const auto start = FunctionStart(address);
			const auto targets = Addresses(a_callees);
			const auto found = core::CountCalledTargets(Code(address, SizeOf(a_fn)), address, targets);
			const bool ok = start != Start::kMidFunction && found >= a_needed;
			Report(a_fn.name, ok, std::format("id {} at game +{:X} ({}): calls {} of the {} expected functions (needs {})", IdOf(a_fn.id), GameOffset(address),
				Describe(start), found, N, a_needed));
			return ok ? address : 0;
		}
	}

	bool SupportedRuntime(const REL::Version& a_version)
	{
		// Capped below 1.8: every id used here was checked present in the address libraries 1.6.317-1.7.104 and
		// 1.5.97 (2026-11-22), and CommonLib ends the game on a missing id instead of reporting it.
		return a_version == REL::Version{ 1, 5, 97, 0 } || (a_version >= REL::Version{ 1, 6, 317, 0 } && a_version < REL::Version{ 1, 8, 0, 0 });
	}

	bool InGameModule(std::uintptr_t a_address)
	{
		HMODULE module = nullptr;
		if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(a_address), &module)) {
			return false;
		}
		return reinterpret_cast<std::uintptr_t>(module) == REL::Module::get().base();
	}

	bool DetourTargetOk(std::uintptr_t a_address, std::string_view a_name)
	{
		const auto start = FunctionStart(a_address);
		const bool ok = start == Start::kYes;
		Report("DETOUR", ok, std::format("{} at game +{:X}: {}", a_name, GameOffset(a_address), Describe(start)));
		return ok;
	}

	std::uintptr_t PreloadItem()
	{
		return CheckByCallees(kPreload, kPreloadCallees, kPreloadNeeded);
	}

	std::uintptr_t ReleaseItem()
	{
		// Drops the preload count (+0x80, lock xadd) and frees the file (+0x88) through its one helper. The helper's SE
		// match is weak (0.445), so on SE the count drop may stand in for it; the members are required either way, and
		// the preload function (which also has a lock xadd) is told apart by its callees.
		const auto address = Address(kRelease.id);
		const auto start = FunctionStart(address);
		const auto code = Code(address, SizeOf(kRelease));
		const std::array<std::uintptr_t, 1> helper{ Address(kReleaseCallee) };
		const bool calls = core::CountCalledTargets(code, address, helper) == 1;
		const bool members = core::HasDisp32(code, 0x80) && core::HasDisp32(code, 0x88);
		const bool countDrop = core::HasLockXadd(code);
		const auto preloadCalls = core::CountCalledTargets(code, address, Addresses(kPreloadCallees));
		const bool ok = start != Start::kMidFunction && members && (calls || (IsSE() && countDrop)) && preloadCalls < kPreloadNeeded;
		Report(kRelease.name, ok, std::format("id {} at game +{:X} ({}): touches +0x80/+0x88 {}, calls its helper {}, lock xadd {}, preload callees {}", IdOf(kRelease.id),
			GameOffset(address), Describe(start), members, calls, countDrop, preloadCalls));
		return ok ? address : 0;
	}

	std::uintptr_t SetCastingArt()
	{
		// A 0x30-byte function: stores the art in the caster (+0xE0), sets a flag (+0xF8), jumps on to 34154. Both
		// members are required; the tail jump too on AE (read in 1.7.104), only logged on SE (its 34154 match is 0.667,
		// while 34150's own match scores 1.0 at the same size).
		const auto address = Address(kSetArt.id);
		const auto start = FunctionStart(address);
		const auto code = Code(address, SizeOf(kSetArt));
		const std::array<std::uintptr_t, 1> tail{ Address(kSetArtTail) };
		const bool jumps = !core::FindBranchesTo(code, address, tail, true).empty();
		const bool members = core::HasDisp32(code, 0xE0) && core::HasDisp32(code, 0xF8);
		const bool ok = start != Start::kMidFunction && members && (jumps || IsSE());
		Report(kSetArt.name, ok, std::format("id {} at game +{:X} ({}): touches +0xE0/+0xF8 {}, jumps on to {} {}", IdOf(kSetArt.id), GameOffset(address), Describe(start),
			members, IdOf(kSetArtTail), jumps));
		return ok ? address : 0;
	}

	std::uintptr_t DetachCastingArt()
	{
		return CheckByCallees(kDetach, kDetachCallees, kDetachNeeded);
	}

	std::uintptr_t ItemChargeReturn(std::uintptr_t a_resourceLookup)
	{
		const auto address = Address(kChargeCaller.id);
		const auto start = FunctionStart(address);
		const auto code = Code(address, SizeOf(kChargeCaller));
		const std::array<std::uintptr_t, 1> lookup{ a_resourceLookup };
		const auto hits = core::FindBranchesTo(code, address, lookup, false);
		const auto own = core::CountCalledTargets(code, address, Addresses(kChargeCallees));
		const bool ok = start != Start::kMidFunction && hits.size() == 1 && own >= kChargeNeeded;
		const auto ret = ok ? address + hits.front().offset + 5 : 0;
		Report(kChargeCaller.name, ok, std::format("id {} at game +{:X} ({}): {} call(s) to the resource lookup, {} of its {} own callees{}", IdOf(kChargeCaller.id),
			GameOffset(address), Describe(start), hits.size(), own, kChargeCallees.size(), ok ? std::format(", return address game +{:X}", GameOffset(ret)) : ""));
		return ret;
	}

	std::uintptr_t FrameCallSite()
	{
		const auto&              version = REL::Module::get().version();
		std::vector<std::size_t> candidates;
		if (IsSE()) {
			candidates = { 0x748 };
		} else if (version >= REL::Version{ 1, 7, 0, 0 }) {
			candidates = { 0xC38, 0xC3D };
		} else if (version >= REL::Version{ 1, 6, 1130, 0 }) {
			candidates = { 0xC26, 0xC2B };  // 0xC26 first: the call proven on 1.6.1170 (21873)
		} else {
			candidates = { 0xC26 };
		}
		const auto  base = Address(kMainUpdate);
		const auto  known = Addresses(kFrameTargets);
		std::string tried;
		for (const auto offset : candidates) {
			const auto site = base + offset;
			const auto code = Code(site, 5);
			if (code[0] != static_cast<std::uint8_t>(core::Branch::kCall)) {
				tried += std::format(" +0x{:X} not a CALL;", offset);
				continue;
			}
			const auto  target = core::Rel32Target(code, site, 0);
			std::string why;
			if (DestinationOk(target, known, kFrameTargets, why)) {
				Report("FRAME", true, std::format("Main::Update +0x{:X} calls {}{}", offset, why, tried.empty() ? "" : "; skipped:" + tried));
				return site;
			}
			tried += std::format(" +0x{:X} calls {};", offset, why);
		}
		Report("FRAME", false, std::format("no candidate in Main::Update (game +{:X}) reaches a known per-frame function:{}", GameOffset(base), tried));
		return 0;
	}

	bool MenuCallSiteOk(std::uintptr_t a_site)
	{
		const auto code = Code(a_site, 5);
		if (code[0] != static_cast<std::uint8_t>(core::Branch::kCall)) {
			Report("MENU", false, std::format("site game +{:X} is not a CALL (opcode {:02X})", GameOffset(a_site), code[0]));
			return false;
		}
		const auto  target = core::Rel32Target(code, a_site, 0);
		std::string why;
		bool        ok = false;
		if (IsSE() && InGameModule(target)) {
			// SE: the 38928 match is weak (0.31); a stray E8 would land inside some function, a real call on a start.
			const auto start = FunctionStart(target);
			ok = start == Start::kYes;
			// Logged so SE players' logs can confirm the weak match afterwards (reviewer 2026-11-22).
			why = std::format("game +{:X}, {}; {} the SE match 37973", GameOffset(target), Describe(start), target == Address(kMenuEquipTarget) ? "equals" : "differs from");
		} else {
			const std::array<std::uintptr_t, 1> known{ Address(kMenuEquipTarget) };
			const std::array<Ids, 1>            knownIds{ kMenuEquipTarget };
			ok = DestinationOk(target, known, knownIds, why);
		}
		Report("MENU", ok, std::format("site game +{:X} calls {}", GameOffset(a_site), why));
		return ok;
	}

	bool VtableSlotsOk()
	{
		const auto tableAddress = [](Table a_table) -> std::uintptr_t {
			switch (a_table) {
			case Table::kEnchantmentItem:
				return REL::Relocation<std::uintptr_t>{ RE::VTABLE_EnchantmentItem[0] }.address();
			case Table::kActorMagicCaster:
				return REL::Relocation<std::uintptr_t>{ RE::VTABLE_ActorMagicCaster[0] }.address();
			case Table::kCharacterMagicTarget:
				return REL::Relocation<std::uintptr_t>{ RE::VTABLE_Character[4] }.address();
			default:
				return REL::Relocation<std::uintptr_t>{ RE::VTABLE_PlayerCharacter[4] }.address();
			}
		};
		const auto slotValue = [&](const VSlot& a_slot) {
			return *reinterpret_cast<const std::uintptr_t*>(tableAddress(a_slot.table) + a_slot.slot * sizeof(std::uintptr_t));
		};
		bool all = true;
		for (const auto& slot : kVSlots) {
			const auto  value = slotValue(slot);
			bool        ok = false;
			std::string why;
			if (!InGameModule(value)) {
				ok = IsExecutable(value);
				why = ok ? "code outside SkyrimSE.exe: another plugin replaced it" : "outside SkyrimSE.exe and not executable memory";
			} else if (IsSE() && slot.expected.se == 0) {
				// No SE match: both MagicTarget slots must hold one function, at a function start.
				const auto other = slotValue(kVSlots[slot.table == Table::kCharacterMagicTarget ? 9 : 8]);
				const auto start = FunctionStart(value);
				ok = start == Start::kYes && (value == other || !InGameModule(other));
				why = std::format("game +{:X}, {}, {} the other MagicTarget slot", GameOffset(value), Describe(start), value == other ? "same as" : "differs from");
			} else {
				ok = value == Address(slot.expected);
				why = std::format("game +{:X}, {} function {}", GameOffset(value), ok ? "the expected" : "NOT the expected", IdOf(slot.expected));
			}
			Report("VTABLE", ok, std::format("table {} slot 0x{:X}: {}", static_cast<int>(slot.table), slot.slot, why));
			all = all && ok;
		}
		return all;
	}
}
