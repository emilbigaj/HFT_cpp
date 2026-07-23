#pragma once

#include <string>
#include <functional>
#include <vector>
#include <mutex>
#include <algorithm>
#include <iostream>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <stdexcept>

namespace Tools
{
	class Application final
	{
	public:
		class ExitAction final
		{
		public:
			std::string Name;
			std::function<void()> Action;
			int Priority;

			ExitAction(const std::string& name, int priority, const std::function<void()>& action) : Name(name), Action(action), Priority(priority) {}
		};

	private:
		static inline std::vector<ExitAction> s_actions;
		static inline std::mutex s_mutex;
		static inline std::atomic<int> s_exiting{ 0 };

		static void SignalHandler(int signal)
		{
			std::cout << "Terminal Signal (Ctrl+C) Captured. Shutting down...\n";
			OnExit();
			std::exit(signal);
		}

        struct AutoInit
        {
            AutoInit() { Application::Init(); }
        };
	    static inline AutoInit s_autoInit;

        static void Init()
		{
			std::signal(SIGINT, SignalHandler);
			std::signal(SIGTERM, SignalHandler);
		}

	public:
		Application() = delete;

		static bool IsExiting()
		{
			return s_exiting.load(std::memory_order_acquire) == 1;
		}

		

		static void AddExitAction(const std::string& name, const std::function<void()>& action)
		{
			AddExitAction(name, 0, action);
		}

		static void AddExitAction(const std::string& name, int priority, const std::function<void()>& action)
		{
			if (name.empty() || name.find_first_not_of(" \t\n\r") == std::string::npos)
				throw std::invalid_argument("Name cannot be null or whitespace.");

			if (!action)
				throw std::invalid_argument("Action cannot be null.");

			std::lock_guard<std::mutex> lock(s_mutex);
			s_actions.emplace_back(name, priority, action);
		}

		static void OnExit()
		{
			if (s_exiting.exchange(1, std::memory_order_acq_rel) == 1)
				return;

			std::cout << "Application shutdown initiated. Running cleanup actions...\n";

			std::vector<ExitAction> snapshot;

			{
				std::lock_guard<std::mutex> lock(s_mutex);
				snapshot = s_actions;
			}

			std::sort(snapshot.begin(), snapshot.end(), [](const ExitAction& a, const ExitAction& b)
			{
				return a.Priority > b.Priority;
			});

			for (const ExitAction& act : snapshot)
			{
				try
				{
					if (act.Action)
						act.Action();
				}
				catch (const std::exception& ex)
				{
					std::cerr << "Exit action '" << act.Name << "' failed: " << ex.what() << std::endl;
				}
				catch (...)
				{
					std::cerr << "Exit action '" << act.Name << "' failed with unknown error." << std::endl;
				}
			}
		}
	};
}