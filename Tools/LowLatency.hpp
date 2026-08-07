#pragma once

#include <algorithm>
#include <cstdlib>
#include <future>
#include <vector>
#include <stdexcept>
#include <mutex>
#include <fstream>
#include <string>
#include <thread>
#include <cstdint>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <csignal>
#include <iostream>
#include <functional>
#include <sched.h>

namespace Tools
{

class LowLatency
{
private:
	inline static std::mutex s_lock;

	inline static const int32_t s_threadPriorityNormal = 0;
	inline static const int32_t s_schedFifo = 1;
	inline static const int32_t s_fifoMaxPriority = 99;

	static int32_t GetCoreCount()
	{
		try
		{
			std::ifstream file("/sys/devices/system/cpu/present");
			std::string content;

			if (std::getline(file, content))
			{
				const size_t dashPos = content.find('-');

				if (dashPos != std::string::npos)
				{
					const int32_t cores = std::stoi(content.substr(dashPos + 1)) + 1;

					return cores;
				}
			}
		}
		catch (const std::exception&)
		{
			return static_cast<int32_t>(std::thread::hardware_concurrency());
		}

		return static_cast<int32_t>(std::thread::hardware_concurrency());
	}

public:
	inline static std::vector<int32_t> BackgroundCores = { 0, 1, 2, 3 };

	inline static const int32_t CoreCount = GetCoreCount();

    static std::thread StartBackgroundThread(const std::string& name, const std::function<void()>& action)
    {
        // The C++ equivalent of ManualResetEventSlim
        std::promise<void> startedPromise;
        std::future<void> startedFuture = startedPromise.get_future();

        std::cout << "Starting background thread: " << name << std::endl;

        std::thread thread([name, action, &startedPromise]()
        {
            PinCurrentThreadToCoreRange(BackgroundCores);
            SetThreadPriorityNormal();
            startedPromise.set_value();
            action();
        });
        // Set the thread name natively (Linux limits thread names to 15 chars + null terminator)
        pthread_setname_np(thread.native_handle(), name.substr(0, 15).c_str());
        startedFuture.wait();

        return thread;
    }

    static void PinCurrentThreadToCoreRange(const std::vector<int32_t>& cores)
    {
        if (cores.empty())
        {
            throw std::invalid_argument("Cores vector cannot be empty.");
        }

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        for (int32_t core : cores)
        {
            CPU_SET(core, &cpuset);
        }

        // Passing 0 as the first argument targets the calling thread
        if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0)
        {
            throw std::runtime_error("Failed to set thread affinity.");
        }
    }

	static void BlockInterruptSignalsCurrentThread()
	{
		sigset_t set;
		sigemptyset(&set);

		sigaddset(&set, SIGHUP);
		sigaddset(&set, SIGINT);
		sigaddset(&set, SIGQUIT);
		sigaddset(&set, SIGTERM);

		const int32_t result = pthread_sigmask(SIG_BLOCK, &set, nullptr);

		if (result != 0)
		{
			std::cout << "WARNING: Failed to set pthread_sigmask. Error: " << result << std::endl;
		}
	}

	inline static thread_local int32_t CurrentCoreId = -1;
	static void PinCurrentThreadToCore(int32_t core)
	{
		if (core < 0 || core >= CoreCount)
		{
			throw std::out_of_range("core");
		}

		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(core, &cpuset);

		if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0)
		{
			throw std::runtime_error("Failed to set affinity.");
		}

		CurrentCoreId = core;   // cache for hot-path reads; invariant once pinned

		BlockInterruptSignalsCurrentThread();
		SetThreadPriorityCritical();
	}

	// The cores that share a last-level cache with this one — on an AMD part its core complex, four
	// cores and one cache slice; on an Intel part its socket. The threads of one pipeline belong on
	// one of these lists: every hand-off between them then stays in that cache instead of crossing
	// the interconnect. Empty when the kernel publishes no cache topology, so a caller checking an
	// allocation against it can tell "not on this complex" from "nothing to check against".
	static std::vector<int32_t> GetCoresSharingLastLevelCache(int32_t core)
	{
		// Step 1: Walk this core's cache levels and keep the list from the deepest one the kernel names.
		std::string sharedCoreList;
		int32_t deepestLevel = 0;
		for (int32_t index = 0; index < 16; ++index)
		{
			const std::string directoryPath = "/sys/devices/system/cpu/cpu" + std::to_string(core) + "/cache/index" + std::to_string(index) + "/";
			std::ifstream levelFile(directoryPath + "level");
			std::string level;
			if (!std::getline(levelFile, level))
				continue;

			std::ifstream sharedFile(directoryPath + "shared_cpu_list");
			std::string shared;
			if (!std::getline(sharedFile, shared))
				continue;

			const int32_t levelNumber = static_cast<int32_t>(std::strtol(level.c_str(), nullptr, 10));
			if (levelNumber <= deepestLevel)
				continue;

			deepestLevel = levelNumber;
			sharedCoreList = shared;
		}

		// Step 2: Expand what the kernel writes — comma-separated cores and dashed ranges, "8-11" or "0,2,4-5".
		std::vector<int32_t> cores;
		size_t at = 0;
		while (at < sharedCoreList.size())
		{
			const size_t comma = std::min(sharedCoreList.find(',', at), sharedCoreList.size());
			const std::string part = sharedCoreList.substr(at, comma - at);
			const size_t dash = part.find('-');
			const int32_t first = static_cast<int32_t>(std::strtol(part.c_str(), nullptr, 10));
			const int32_t last = dash == std::string::npos ? first : static_cast<int32_t>(std::strtol(part.c_str() + dash + 1, nullptr, 10));
			for (int32_t sharing = first; sharing <= last; ++sharing)
				cores.push_back(sharing);
			at = comma + 1;
		}
		return cores;
	}


	static void SetThreadPriorityCritical()
	{
        sched_param parameter;
        parameter.sched_priority = s_fifoMaxPriority;

        if (sched_setscheduler(0, s_schedFifo, &parameter) != 0)
        {
            throw std::runtime_error("Failed to set SCHED_FIFO priority.");
        }
	}

	static void SetThreadPriorityNormal()
	{
        sched_param parameter;
        parameter.sched_priority = s_threadPriorityNormal;

        if (sched_setscheduler(0, SCHED_OTHER, &parameter) != 0)
        {
            throw std::runtime_error("Failed to set SCHED_OTHER priority.");
        }
	}

    static int32_t GetCurrentCoreId()
    {
        return sched_getcpu();
    }

    static std::string GetCurrentThreadName()
    {
        // Linux thread names are limited to 16 bytes (15 characters + '\0')
        char buffer[16];
        
        int result = pthread_getname_np(pthread_self(), buffer, sizeof(buffer));

        if (result == 0)
            return std::string(buffer);

        return std::string();
    }
};
}