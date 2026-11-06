#pragma once
#include <cstdint>
namespace SpellCastingStaff::InputSnapshotPolicy {
// A staged button snapshot is valid only if a newer native attack event has
// not been processed. State codes: 0=empty, 1=released, 2=pressed.
[[nodiscard]] constexpr std::uint64_t Pack(std::uint64_t nativeSequence, bool pressed) noexcept {
    return (nativeSequence << 2) | (pressed ? 2u : 1u);
}
[[nodiscard]] constexpr bool Read(std::uint64_t packed, std::uint64_t currentNativeSequence,
                                  bool& pressed) noexcept {
    if (!packed || (packed >> 2) != currentNativeSequence) return false;
    const auto state = packed & 3u;
    if (state != 1u && state != 2u) return false;
    pressed = state == 2u;
    return true;
}
} // namespace SpellCastingStaff::InputSnapshotPolicy
