#pragma once

// Development-only controls so an agent can run the casting tests without a mouse or the magic menu.
// Active only with [SelfTest] bEnabled=1 and bTestControls=1. Triggered by key presses that DevBench
// injects (keyboard only):
//   F9  = bind the charge spell of the staff in the right hand, through the same call SCS intercepts
//         when the magic menu equips a spell (SCS MenuEquipHook.cpp: call at 37952/38908 + 0xD7)
//   F10 = one right-hand attack: press, hold for iTestHoldMs, release ("Right Attack/Block" button events)
//   F11 = same for the left hand
//   F9 on an enchanted staff we do not convert = next step of the enchantment-copy spike (testcontrols.cpp)
// Nothing here runs in a normal game.

namespace scssc::testcontrols
{
	void Install();                // input sink; call at kDataLoaded
	void ResetSpike();             // forget the spike's item and step; call when a save starts loading or a new game starts
	void OnFrame(float a_delta);  // drives a running attack hold; called from the frame hook
	void LogSaveState(const char* a_when);  // what the hand slot and active effects reference; call at kSaveGame and kPostLoadGame

	// T-SAVE fix under test: keeps a runtime copy out of the right-hand slot while a save is written (not off the
	// item in steps 1-3, nor off casts or projectiles in flight), and judges after a real restart whether the
	// effects the staff produced came back (SELFTEST T-SAVE-COLD).
	inline constexpr std::uint32_t kColdSaveRecord = 'TSAV';
	void PrepareSave();  // call at kSaveGame, after LogSaveState
	void WriteSaveRecord(SKSE::SerializationInterface* a_intfc);
	void ReadSaveRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length);
	void RevertSaveRecord();
	void EvaluateColdLoad(bool a_loadSucceeded);  // call at kPostLoadGame
}
