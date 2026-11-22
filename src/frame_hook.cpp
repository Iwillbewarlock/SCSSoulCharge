#include "frame_hook.h"

#include "cast_watch.h"
#include "main_queue.h"
#include "runtime_check.h"
#include "selftest.h"
#include "settings.h"
#include "spell_copy.h"
#include "staff_art.h"
#include "staff_binding.h"
#include "staff_mark.h"

#include <atomic>
#include <chrono>

namespace scssc::frame_hook
{
	namespace
	{
		REL::Relocation<void (*)()> g_originalFrame;
		std::uintptr_t              g_site = 0;  // set by Prepare
		std::atomic<std::uint32_t>  g_mainThread{ 0 };

		float FrameDelta()
		{
			static const auto address = REL::VariantID(523660, 410199, 0x30C3A08).address();  // the global SCS reads
			return *reinterpret_cast<const float*>(address);
		}

		// T-PERF: what this mod's frame work costs, measured before anything is optimized (user request 2026-11-18). One
		// line per minute of unpaused play, only with [SelfTest] bEnabled: frames, average and worst time, frames spent
		// with a target staff in hand, and how often a runtime form reached the copy table's lock.
		constexpr float  kPerfReportSecs = 60.0f;
		constexpr double kPerfBudgetUs = 50.0;  // FAIL above this average: a 60 fps frame is 16 667 us

		struct Perf
		{
			float         clock{ 0.0f };
			std::uint64_t frames{ 0 };
			std::uint64_t staffFrames{ 0 };
			double        totalUs{ 0.0 };
			double        worstUs{ 0.0 };
		};
		Perf g_perf;

		void Measure(std::chrono::steady_clock::time_point a_start, float a_delta, bool a_staffHeld)
		{
			if (!Settings::Get().selfTest) {
				return;
			}
			const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a_start).count();
			auto&        p = g_perf;
			++p.frames;
			p.staffFrames += a_staffHeld ? 1 : 0;
			p.totalUs += us;
			p.worstUs = std::max(p.worstUs, us);
			if (std::isfinite(a_delta) && a_delta > 0.0f) {
				p.clock += a_delta;
			}
			if (p.clock < kPerfReportSecs || p.frames == 0) {
				return;
			}
			const double average = p.totalUs / static_cast<double>(p.frames);
			const auto   lookups = spell_copy::TakeLockedLookups();
			selftest::Report("T-PERF", average <= kPerfBudgetUs,
				std::format("{} frames in {:.0f} s: average {:.1f} us, worst {:.1f} us, staff in hand {} frames; copy-table lookups (runtime forms) {}", p.frames,
					p.clock, average, p.worstUs, p.staffFrames, lookups));
			p = {};
		}

		void FrameThunk()
		{
			g_originalFrame();
			if (g_mainThread.load() == 0) {
				g_mainThread = REX::W32::GetCurrentThreadId();
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* ui = RE::UI::GetSingleton();
			if (!player || !ui || ui->GameIsPaused()) {
				return;
			}
			const auto start = std::chrono::steady_clock::now();
			main_queue::Drain();
			const auto held = staff_binding::OnFrame(player);
			staff_mark::OnFrame(staff_binding::ActiveSpell(Hand::kRight), staff_binding::ActiveSpell(Hand::kLeft));
			const float delta = FrameDelta();
			for (const auto hand : kHands) {
				staff_art::OnFrame(player, delta, hand, held[Index(hand)]);
			}
			cast_watch::OnFrame(player, delta);
			Measure(start, delta, held[0] || held[1]);
		}

	}

	std::uint32_t MainThreadId()
	{
		return g_mainThread.load();
	}

	bool Prepare()
	{
		g_site = runtime_check::FrameCallSite();
		if (!g_site) {
			logger::critical("No usable Main::Update frame call; the mod stays off");
			return false;
		}
		return true;
	}

	bool Install()
	{
		if (!g_site) {
			return false;
		}
		g_originalFrame = SKSE::GetTrampoline().write_call<5>(g_site, FrameThunk);
		logger::info("Hook installed: Main::Update frame call at {:X}", g_site);
		return true;
	}
}
