#pragma once
#include <array>
#include <cstdint>
namespace SpellCastingStaff {
enum class StaffKind { Basic, Stamina, Swift, Focus, DualCast, Eternity };
struct StaffProfile { std::uint32_t keywordID; StaffKind kind; int tier; };
inline constexpr std::array<StaffProfile, 27> kStaffProfiles{{
    {0x0008EE, StaffKind::Basic, 0}, // SCS_Basic
    {0x0008F6, StaffKind::Basic, 1}, // SCS_Basic01
    {0x0008FF, StaffKind::Basic, 2}, // SCS_Basic02
    {0x000900, StaffKind::Basic, 3}, // SCS_Basic03
    {0x000901, StaffKind::Basic, 4}, // SCS_Basic04
    {0x000902, StaffKind::Basic, 5}, // SCS_Basic05
    {0x0008F9, StaffKind::Stamina, 1}, // SCS_Stamina01
    {0x000903, StaffKind::Stamina, 2}, // SCS_Stamina02
    {0x000904, StaffKind::Stamina, 3}, // SCS_Stamina03
    {0x000905, StaffKind::Stamina, 4}, // SCS_Stamina04
    {0x000906, StaffKind::Stamina, 5}, // SCS_Stamina05
    {0x0008EA, StaffKind::Swift, 2}, // SCS_Swift02
    {0x000907, StaffKind::Swift, 3}, // SCS_Swift03
    {0x000908, StaffKind::Swift, 4}, // SCS_Swift04
    {0x000909, StaffKind::Swift, 5}, // SCS_Swift05
    {0x00090A, StaffKind::Focus, 2}, // SCS_Focus02
    {0x00090B, StaffKind::Focus, 3}, // SCS_Focus03
    {0x00090C, StaffKind::Focus, 4}, // SCS_Focus04
    {0x00090D, StaffKind::Focus, 5}, // SCS_Focus05
    {0x0008F7, StaffKind::DualCast, 3}, // SCS_DualCast03
    {0x00090E, StaffKind::DualCast, 4}, // SCS_DualCast04
    {0x00090F, StaffKind::DualCast, 5}, // SCS_DualCast05
    {0x0009BB, StaffKind::Eternity, 1}, // SCS_Eternity01 (new WEAP 0x9BE)
    {0x0009A8, StaffKind::Eternity, 2}, // SCS_Eternity02 (new WEAP 0x9AA)
    {0x0009A7, StaffKind::Eternity, 3}, // SCS_Eternity03 (new WEAP 0x9A9)
    {0x0008F8, StaffKind::Eternity, 4}, // SCS_Eternity04 (formerly Transcend04)
    {0x000910, StaffKind::Eternity, 5}, // SCS_Eternity05 (formerly Transcend05)
}};
// The native spell effectiveness argument is always neutral (1.0f).
// Staff-specific magnitude bonuses belong exclusively to ESP PERK entries.
inline constexpr float StaffChargeTime(const StaffProfile& p, float base) {
    // Swift borrows Concentration animation, NOT instant charge time.
    if (p.kind == StaffKind::Swift) return base;
    if (p.kind == StaffKind::Focus) return base * 1.5f + 1.0f;
    return base;
}
// Base spell charge metadata is limited to 30 seconds; Focus can reach 46.0.
inline constexpr float kMaxStaffChargeTime = 46.0f;
} // namespace SpellCastingStaff
