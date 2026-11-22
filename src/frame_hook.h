#pragma once

// A Main::Update frame call (runtime_check::FrameCallSite picks it per game build): the main thread, once per frame. Runs
// the main queue, keeps the hand slot on the bound copy, and refreshes the tip art. Nothing runs while the game is
// paused (menus), so a menu pick takes effect on the first frame after the menu closes.

namespace scssc::frame_hook
{
	// Finds and checks the frame call without writing anything; false = the mod stays off.
	bool Prepare();

	// Hooks the call Prepare found.
	bool Install();

	// The thread the frame hook runs on (the main thread), 0 until the first frame. For T-THREAD.
	std::uint32_t MainThreadId();
}
