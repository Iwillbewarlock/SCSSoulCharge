#pragma once
#include <REL/Relocation.h>
#include <cstddef>
#include <cstdint>
#include <string>

#if !defined(ENABLE_SKYRIM_SE) || !defined(ENABLE_SKYRIM_AE)
#error SpellCastingStaff 2.1.1 requires CommonLibSSE-NG built with SE and AE support.
#endif

namespace SpellCastingStaff::RuntimeCompatibility {
    inline bool Supported(const REL::Version& version) {
        return version == REL::Version{1, 5, 97, 0} || version == REL::Version{1, 6, 1170, 0} || version == REL::Version{1, 7, 104, 0};
    }
    inline bool IsAE1170() {
        return REL::Module::get().version() == REL::Version{1, 6, 1170, 0};
    }
    inline bool IsAE17104() {
        return REL::Module::get().version() == REL::Version{1, 7, 104, 0};
    }
    inline bool IsSupportedAE() { return IsAE1170() || IsAE17104(); }
    inline std::size_t AttackButtonSlot() { return IsAE17104() ? 0x06 : 0x04; }
    inline std::size_t FrameCallOffset() { return IsAE17104() ? 0xC38 : (IsAE1170() ? 0xC26 : 0x748); }
    bool ReadableMemory(std::uintptr_t address, std::size_t bytes);
    bool ExecutableTarget(std::uintptr_t address);
    // Best-effort owner of an address; MEM_PRIVATE trampolines cannot reveal their installing DLL.
    std::string DescribeAddress(std::uintptr_t address);
    // Hard gate: only supported runtime + exact AE Address Library mappings.
    // Hook opcodes, CALL targets and vtable entries are checked by each Install().
    bool ValidateRuntimeAndMappings();
}
