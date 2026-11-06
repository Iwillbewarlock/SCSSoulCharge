#pragma once

// Thin wrapper over Microsoft Detours, compiled without the CommonLib PCH (detours.h wants its own
// <Windows.h>). Same pattern as D:\DEV\StaffCasting\src\detour_helper.*. Chains with other plugins'
// Detours hooks on the same function.
namespace scssc::detour
{
	// a_target holds the function address; on success it becomes the trampoline that runs the original.
	// Returns the Detours error code (0 = NO_ERROR).
	long Attach(void** a_target, void* a_detour) noexcept;
}
