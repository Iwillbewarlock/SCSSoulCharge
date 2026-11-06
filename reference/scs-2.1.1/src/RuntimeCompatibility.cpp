#include "SpellCastingStaff/RuntimeCompatibility.h"
#include <array>
#include <string>
#include <string_view>
#include <iterator>
#include <spdlog/spdlog.h>
#include <Windows.h>

namespace SpellCastingStaff::RuntimeCompatibility {
    bool ReadableMemory(std::uintptr_t address, std::size_t bytes) {
        MEMORY_BASIC_INFORMATION info{};
        if (!bytes || !VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info))) return false;
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        return address >= start && address - start < info.RegionSize &&
               bytes <= info.RegionSize - (address - start);
    }

    bool ExecutableTarget(std::uintptr_t address) {
        MEMORY_BASIC_INFORMATION info{};
        if (!address || !VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info))) return false;
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const auto protection = info.Protect & 0xFF;
        return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
               protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    }
std::string DescribeAddress(std::uintptr_t address) {
    if (!address) return "null";
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) && module) {
        wchar_t name[1024]{};
        const auto len = GetModuleFileNameW(module, name, static_cast<DWORD>(std::size(name)));
        if (len && len < std::size(name)) {
            const std::wstring_view full(name, len);
            const auto slash = full.find_last_of(L"\\/");
            const auto baseName = full.substr(slash == std::wstring_view::npos ? 0 : slash + 1);
            const int required = WideCharToMultiByte(CP_UTF8, 0, baseName.data(),
                static_cast<int>(baseName.size()), nullptr, 0, nullptr, nullptr);
            if (required > 0) {
                std::string utf8(required, '\0');
                WideCharToMultiByte(CP_UTF8, 0, baseName.data(),
                    static_cast<int>(baseName.size()), utf8.data(), required, nullptr, nullptr);
                return utf8;
            }
        }
        return "mapped PE module (filename unavailable)";
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info))) {
        return info.Type == MEM_PRIVATE ? "MEM_PRIVATE (trampoline / owner unknown)" :
               info.Type == MEM_MAPPED ? "MEM_MAPPED (owner unknown)" :
               "module unknown";
    }
    return "unmapped/unknown";
}

bool ValidateRuntimeAndMappings() {
    if (!Supported(REL::Module::get().version())) return false;
    if (!IsSupportedAE()) return true; // Existing SE installers retain their own guards.
    const auto base = REL::Module::get().base();
    struct Mapping { std::uint64_t id; std::uintptr_t rva; };
    constexpr std::array<Mapping, 11> mappings1170{{
        {36564, 0x645EA0}, {38908, 0x6CA920}, {38894, 0x6C9820},
        {38906, 0x6CA4D0}, {34227, 0x5B4D10}, {38928, 0x6CBAD0},
        {410199, 0x31CC288}, {205828, 0x187B8B0}, {205842, 0x187BBE8},
        {208719, 0x18B6DF0}, {208050, 0x18AC478}
    }};
    constexpr std::array<Mapping, 11> mappings17104{{
        {36564, 0x658870}, {38908, 0x6DD410}, {38894, 0x6DC310},
        {38906, 0x6DCFC0}, {34227, 0x5C3850}, {38928, 0x6DE5C0},
        {410199, 0x3275608}, {205828, 0x18F6150}, {205842, 0x18F6488},
        {208719, 0x1935258}, {208050, 0x192A178}
    }};
    const auto& mappings = IsAE17104() ? mappings17104 : mappings1170;
    for (const auto& map : mappings) {
        const auto address = REL::ID(map.id).address();
        if (address != base + map.rva) {
            SKSE::log::critical("SCS rejected Address Library mapping id={} rva={:X} expected={:X}; no hooks installed",
                map.id, address - base, map.rva);
            return false;
        }
    }

    // Never reject the entire DLL for a native CALL target or vtable that
    // another SKSE plugin hooked first. Each hook checks memory, opcode and
    // executable target immediately before its own patching.
    // Address Library identity is the only AE-specific global hard gate.
    SKSE::log::warn("SCS MAPPING PASS: 11 Address Library IDs verified; "
                    "native hook sites checked independently at Install (not an in-game validation)");
    return true;
}
}
