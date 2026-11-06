#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
namespace SpellCastingStaff {
struct StopHands { bool left; bool right; };
inline constexpr StopHands AnimationStopPolicy(std::string_view tag) {
    if (tag == "MLh_SpellStop_Event" || tag == "MLh_SpellInterrupt_Event") return {true, false};
    if (tag == "MRh_SpellStop_Event" || tag == "MRh_SpellInterrupt_Event") return {false, true};
    if (tag == "Spell_Stop" || tag == "Spell_Interrupt" || tag == "InterruptCast" || tag == "weapSheathe") return {true, true};
    return {false, false};
}
inline float UnpaidEscrow(float cost, float paid) {
    if (!std::isfinite(cost) || !std::isfinite(paid) || cost < 0.0f || paid < 0.0f)
        return std::numeric_limits<float>::quiet_NaN();
    return std::max(0.0f, cost - paid);
}
struct EscrowSettlement { bool success; float due; float available; };
template<class Read, class Debit>
EscrowSettlement CollectEscrowBalance(float cost, float& paid, Read read, Debit debit) {
    const float due = UnpaidEscrow(cost, paid);
    const float before = read();
    if (!std::isfinite(due) || !std::isfinite(before) || before + 0.001f < due)
        return {false, due, before};
    if (due == 0.0f) return {true, due, before};
    debit(due);
    const float after = read();
    if (!std::isfinite(after)) return {false, due, after};
    paid += std::clamp(before - after, 0.0f, due);
    return {paid + 0.001f >= cost, due, after};
}

}
