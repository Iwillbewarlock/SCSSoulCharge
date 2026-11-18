#pragma once

// The Main::Update frame call (the site SCS also uses, FrameHook.cpp:39-63): the main thread, once per frame. Runs
// the main queue, keeps the hand slot on the bound copy, and refreshes the tip art. Nothing runs while the game is
// paused (menus), so a menu pick takes effect on the first frame after the menu closes.

namespace scssc::frame_hook
{
	bool Install();

	// The thread the frame hook runs on (the main thread), 0 until the first frame. For T-THREAD.
	std::uint32_t MainThreadId();
}
