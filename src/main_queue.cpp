#include "main_queue.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace scssc::main_queue
{
	namespace
	{
		struct Task
		{
			std::uint32_t         generation;
			std::function<void()> run;
		};

		// One global lock: the queue holds a few tasks per menu pick at most.
		std::atomic<std::uint32_t> g_generation{ 0 };
		std::mutex                 g_lock;
		std::vector<Task>          g_tasks;
	}

	std::uint32_t Generation()
	{
		return g_generation.load();
	}

	void NextGeneration()
	{
		g_generation.fetch_add(1);
	}

	void Post(std::function<void()> a_task)
	{
		const std::scoped_lock lock{ g_lock };
		g_tasks.push_back({ g_generation.load(), std::move(a_task) });
	}

	void Drain()
	{
		std::vector<Task> tasks;
		{
			const std::scoped_lock lock{ g_lock };
			tasks.swap(g_tasks);
		}
		const auto now = g_generation.load();
		std::size_t dropped = 0;
		for (auto& task : tasks) {
			if (task.generation != now) {
				++dropped;
				continue;
			}
			task.run();
		}
		if (dropped > 0) {
			logger::info("main queue: dropped {} task(s) posted before a load", dropped);
		}
	}
}
