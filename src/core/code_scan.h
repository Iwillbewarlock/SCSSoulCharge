#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Byte-level checks on game code, used at start to confirm that an address-library id points at the function this
// mod expects on the running game version (SE 1.5.97, every AE build). No disassembler: a CALL (E8) or JMP (E9) with a
// 32-bit displacement is found by trying every byte. A stray E8/E9 inside another instruction gives a random
// destination, so it only counts when that destination is one of the expected functions; that is what makes these
// checks specific. Game-independent so the offline tests can feed it byte buffers.

namespace scssc::core
{
	enum class Branch : std::uint8_t
	{
		kCall = 0xE8,
		kJump = 0xE9,
	};

	struct BranchHit
	{
		std::size_t   offset{ 0 };  // where the opcode byte sits, from the start of the scanned code
		std::uintptr_t target{ 0 };
		Branch        kind{ Branch::kCall };
	};

	// Destination of the rel32 branch whose opcode is at a_code[a_offset], for code that lives at a_codeAddress.
	// 0 when there is no room for the displacement.
	[[nodiscard]] std::uintptr_t Rel32Target(std::span<const std::uint8_t> a_code, std::uintptr_t a_codeAddress, std::size_t a_offset);

	// Every E8 (and E9 when a_jumpsToo) in a_code whose destination is one of a_targets. O(size * targets).
	[[nodiscard]] std::vector<BranchHit> FindBranchesTo(std::span<const std::uint8_t> a_code, std::uintptr_t a_codeAddress,
		std::span<const std::uintptr_t> a_targets, bool a_jumpsToo);

	// How many different members of a_targets are called (E8) somewhere in a_code.
	[[nodiscard]] std::size_t CountCalledTargets(std::span<const std::uint8_t> a_code, std::uintptr_t a_codeAddress, std::span<const std::uintptr_t> a_targets);

	// Whether a_bytes appear in a_code, in order and next to each other.
	[[nodiscard]] bool ContainsBytes(std::span<const std::uint8_t> a_code, std::span<const std::uint8_t> a_bytes);

	// Whether a "lock xadd r/m32, r32" (F0 0F C1) appears, also with a REX prefix between LOCK and the opcode
	// (F0 4x 0F C1, registers r8-r15): how the engine drops a reference count.
	[[nodiscard]] bool HasLockXadd(std::span<const std::uint8_t> a_code);

	// Whether the 4 little-endian bytes of a_disp appear in a_code (a [reg+disp32] operand, e.g. a member at +0x80).
	[[nodiscard]] bool HasDisp32(std::span<const std::uint8_t> a_code, std::uint32_t a_disp);
}
