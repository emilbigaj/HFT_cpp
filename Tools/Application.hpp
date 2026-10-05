#pragma once

#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <mutex>
#include <algorithm>
#include <iostream>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>

namespace Tools
{
	class Application final
	{
	public:
		class ExitAction final
		{
		public:
			const std::string Name;
			const int Priority;
			const std::function<void()> Action;

			ExitAction(const std::string& name, int priority, const std::function<void()>& action) : Name(name), Priority(priority), Action(action) {}

			bool IsCancelled() const
			{
				return _cancelled.load(std::memory_order_acquire);
			}

			// The owner already cleaned up (or is being destroyed): OnExit skips this action.
			void Cancel()
			{
				_cancelled.store(true, std::memory_order_release);
			}

		private:
			std::atomic<bool> _cancelled{ false };
		};

	private:
		static inline std::vector<std::shared_ptr<ExitAction>> s_actions;
		static inline std::mutex s_mutex;
		static inline std::atomic<int> s_exiting{ 0 };
		static inline std::atomic<bool> s_exited{ false };

		static void SignalHandler(int signal)
		{
			if (signal == SIGHUP)
				std::cout << "Hang-up Signal (SIGHUP) Captured. Shutting down...\n";
			else
				std::cout << "Terminal Signal (Ctrl+C) Captured. Shutting down...\n";
			OnExit();

			// Like C#'s Cancel = false: the default action ends the process once the handler returns,
			// without flushing stdio.
			std::cout.flush();
			std::fflush(nullptr);
			std::signal(signal, SIG_DFL);
			std::raise(signal);
		}

        struct AutoInit
        {
            AutoInit() { Application::Init(); }
        };
	    static inline AutoInit s_autoInit;

        static void Init()
		{
			// All three are masked while any handler runs: a nested handler on the same thread would
			// otherwise wait in OnExit for the chain it interrupted.
			struct sigaction action{};
			action.sa_handler = SignalHandler;
			sigemptyset(&action.sa_mask);
			sigaddset(&action.sa_mask, SIGINT);
			sigaddset(&action.sa_mask, SIGTERM);
			sigaddset(&action.sa_mask, SIGHUP);
			action.sa_flags = SA_RESTART;
			sigaction(SIGINT, &action, nullptr);
			sigaction(SIGTERM, &action, nullptr);
			sigaction(SIGHUP, &action, nullptr);
			std::atexit([] { Application::OnExit(); });
		}

	public:
		Application() = delete;

		static bool IsExiting()
		{
			return s_exiting.load(std::memory_order_acquire) == 1;
		}

		static std::shared_ptr<ExitAction> AddExitAction(const std::string& name, const std::function<void()>& action)
		{
			return AddExitAction(name, 0, action);
		}

		// Higher priority runs earlier. Cancel the returned action once its owner is gone.
		static std::shared_ptr<ExitAction> AddExitAction(const std::string& name, int priority, const std::function<void()>& action)
		{
			if (name.empty() || name.find_first_not_of(" \t\n\r") == std::string::npos)
				throw std::invalid_argument("Name cannot be null or whitespace.");

			if (!action)
				throw std::invalid_argument("Action cannot be null.");

			std::shared_ptr<ExitAction> exitAction = std::make_shared<ExitAction>(name, priority, action);
			std::lock_guard<std::mutex> lock(s_mutex);
			s_actions.push_back(exitAction);
			return exitAction;
		}

		static void OnExit()
		{
			// A later caller blocks until the first caller's chain completes: returning at once would
			// let it run exit/static destructors under actions still in progress on the other thread.
			if (s_exiting.exchange(1, std::memory_order_acq_rel) == 1)
			{
				s_exited.wait(false, std::memory_order_acquire);
				return;
			}

			std::cout << "Application::OnExit() - Running cleanup actions...\n";

			std::vector<std::shared_ptr<ExitAction>> snapshot;

			{
				std::lock_guard<std::mutex> lock(s_mutex);
				snapshot = s_actions;
			}

			std::stable_sort(snapshot.begin(), snapshot.end(), [](const std::shared_ptr<ExitAction>& a, const std::shared_ptr<ExitAction>& b)
			{
				return a->Priority > b->Priority;
			});

			for (const std::shared_ptr<ExitAction>& act : snapshot)
			{
				if (act->IsCancelled())
					continue;   // its owner already cleaned up
				try
				{
					std::cout << "Application::Executing exit action '" << act->Name << "' with priority " << act->Priority << "...\n";
					act->Action();
				}
				catch (const std::exception& ex)
				{
					std::cerr << "Exit action '" << act->Name << "' failed: " << ex.what() << std::endl;
				}
				catch (...)
				{
					std::cerr << "Exit action '" << act->Name << "' failed with unknown error." << std::endl;
				}
			}

			s_exited.store(true, std::memory_order_release);
			s_exited.notify_all();
		}
	};
}
