#include "core/code_scan.h"

#include <algorithm>
#include <cstring>

namespace scssc::core
{
	namespace
	{
		constexpr std::size_t kBranchSize = 5;  // opcode + rel32
	}

	std::uintptr_t Rel32Target(std::span<const std::uint8_t> a_code, std::uintptr_t a_codeAddress, std::size_t a_offset)
	{
		if (a_offset > a_code.size() || a_code.size() - a_offset < kBranchSize) {
			return 0;
		}
		std::int32_t displacement = 0;
		std::memcpy(&displacement, a_code.data() + a_offset + 1, sizeof(displacement));
		return a_codeAddress + a_offset + kBranchSize + static_cast<std::intptr_t>(displacement);
	}

	std::vector<BranchHit> FindBranchesTo(std::span<const std::uint8_t> a_code, std::uintptr_t a_codeAddress, std::span<const std::uintptr_t> a_targets,
		bool a_jumpsToo)
	{
		std::vector<BranchHit> hits;
		if (a_code.size() < kBranchSize) {
			return hits;
		}
		for (std::size_t i = 0; i + kBranchSize <= a_code.size(); ++i) {
			const auto op = a_code[i];
			if (op != static_cast<std::uint8_t>(Branch::kCall) && !(a_jumpsToo && op == static_cast<std::uint8_t>(Branch::kJump))) {
				continue;
			}
			const auto target = Rel32Target(a_code, a_codeAddress, i);
			if (std::ranges::find(a_targets, target) != a_targets.end()) {
				hits.push_back({ i, target, static_cast<Branch>(op) });
			}
		}
		return hits;
	}

	std::size_t CountCalledTargets(std::span<const std::uint8_t> a_code, std::uintptr_t a_codeAddress, std::span<const std::uintptr_t> a_targets)
	{
		const auto            hits = FindBranchesTo(a_code, a_codeAddress, a_targets, false);
		std::vector<std::uintptr_t> seen;
		for (const auto& hit : hits) {
			if (std::ranges::find(seen, hit.target) == seen.end()) {
				seen.push_back(hit.target);
			}
		}
		return seen.size();
	}

	bool ContainsBytes(std::span<const std::uint8_t> a_code, std::span<const std::uint8_t> a_bytes)
	{
		return !a_bytes.empty() && !std::ranges::search(a_code, a_bytes).empty();
	}

	bool HasLockXadd(std::span<const std::uint8_t> a_code)
	{
		for (std::size_t i = 0; i < a_code.size(); ++i) {
			if (a_code[i] != 0xF0) {
				continue;
			}
			std::size_t op = i + 1;
			if (op < a_code.size() && (a_code[op] & 0xF0) == 0x40) {
				++op;  // REX
			}
			if (op + 2 <= a_code.size() && a_code[op] == 0x0F && a_code[op + 1] == 0xC1) {
				return true;
			}
		}
		return false;
	}

	bool HasDisp32(std::span<const std::uint8_t> a_code, std::uint32_t a_disp)
	{
		std::uint8_t bytes[4];
		std::memcpy(bytes, &a_disp, sizeof(bytes));
		return ContainsBytes(a_code, bytes);
	}
}
