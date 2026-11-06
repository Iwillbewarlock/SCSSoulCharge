#include "detour_helper.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <detours/detours.h>

namespace scssc::detour
{
	long Attach(void** a_target, void* a_detour) noexcept
	{
		if (const auto err = DetourTransactionBegin(); err != NO_ERROR) {
			return err;
		}
		DetourUpdateThread(GetCurrentThread());
		if (const auto err = DetourAttach(a_target, a_detour); err != NO_ERROR) {
			DetourTransactionAbort();
			return err;
		}
		return DetourTransactionCommit();
	}
}
