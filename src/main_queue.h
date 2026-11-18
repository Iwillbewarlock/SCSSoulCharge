#pragma once

#include <cstdint>
#include <functional>

// Work that changes engine state (hand slot, preload, casting art) runs only here: at the top of the Main::Update
// frame call (frame_hook.cpp FrameThunk), which is the main thread. SKSE's AddTask runs on job worker threads during
// normal play (HoM Black Flame MainThreadQueue.h: 409 of 415 task bodies off the main thread).

namespace scssc::main_queue
{
	// Counts up on every load and new game. A task posted before that is dropped (its owner is gone).
	std::uint32_t Generation();
	void          NextGeneration();

	// a_task runs on the main thread at the next unpaused frame, unless a load came in between.
	void Post(std::function<void()> a_task);

	// Called by the frame hook only.
	void Drain();
}
