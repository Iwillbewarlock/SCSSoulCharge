#pragma once

namespace SpellCastingStaff::FrameHook {
    // Same main loop relocation/call site as Spell Hotbar 2 for SE 1.5.97.
    // Must chain the original call. Do not install twice.
    bool Install();
}
