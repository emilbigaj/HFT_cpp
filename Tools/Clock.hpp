#pragma once

#include "Application.hpp"
#include "Event.hpp"
#include "LockedPriorityQueue.hpp"
#include "Timestamp.hpp"

#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <immintrin.h>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace Tools
{
	enum class ClockMode : uint8_t
	{
		Simulation = 0,
		Realtime = 1,
	};

	struct Reminder
	{
		Tools::Timestamp Timestamp;
		// Shared so copies of one Reminder keep the callback's identity, which equality compares (C# ReferenceEquals).
		std::shared_ptr<const std::function<void(Tools::Timestamp)>> Callback;
		std::string Name;

		Reminder() = default;

		Reminder(Tools::Timestamp timestamp, std::function<void(Tools::Timestamp)> callback, std::string name = "")
			: Timestamp(timestamp), Callback(std::make_shared<const std::function<void(Tools::Timestamp)>>(std::move(callback))), Name(std::move(name)) {}

		// Reminders built from one held callback compare equal, like C# Reminders built from one stored delegate.
		Reminder(Tools::Timestamp timestamp, std::shared_ptr<const std::function<void(Tools::Timestamp)>> callback, std::string name = "")
			: Timestamp(timestamp), Callback(std::move(callback)), Name(std::move(name)) {}

		friend bool operator==(const Reminder& a, const Reminder& b)
		{
			return a.Timestamp == b.Timestamp && a.Callback == b.Callback;
		}

		std::string ToString() const
		{
			return "Reminder " + Name + " " + Timestamp.ToString();
		}
	};

	class Clock final
	{
	private:
		static inline LockedPriorityQueue<Tools::Timestamp, Reminder> s_reminders;

		static inline std::atomic<bool> s_isRunning{ false };
		static inline std::atomic<bool> s_isStopping{ false };

		static inline ClockMode s_mode = ClockMode::Simulation;

		// Atomic because UtcNow() reads them from any thread while the clock thread advances them.
		static inline std::atomic<Tools::Timestamp> s_simulationNow{ Tools::Timestamp::MinValue };
		static inline std::atomic<Tools::Timestamp> s_interjection{ Tools::Timestamp::MinValue };
		static inline Tools::Timestamp s_begin = Tools::Timestamp::MinValue;
		static inline Tools::Timestamp s_end = Tools::Timestamp::MaxValue;

		static inline std::mutex s_lock;
		static inline std::atomic<std::thread::id> s_runningThreadId{};

		static inline std::atomic<double> s_simulationSpeed{ std::numeric_limits<double>::max() };
		static inline int64_t s_anchorWallTicks = 0;
		static inline Tools::Timestamp s_anchorSimTime = Tools::Timestamp::MinValue;
		// Raised by the setter, cleared when the clock thread applies the speed: a pacing wait in progress gives up at once.
		static inline std::atomic<bool> s_isSimulationSpeedChanging{ false };

		static int64_t GetWallTicks()
		{
			return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		static constexpr int64_t s_wallTicksPerSecond = 1'000'000'000;

		static void Init()
		{
			s_simulationNow.store(s_begin, std::memory_order_relaxed);

			Application::AddExitAction("Stop Clock", INT_MAX, []()
			{
				Stop();
				// A signal handled on the clock's own thread would otherwise wait on itself forever.
				if (s_runningThreadId.load() == std::this_thread::get_id())
					return;
				while (IsRunning())
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			});
		}

		struct AutoInit
		{
			AutoInit() { Clock::Init(); }
		};
		static inline AutoInit s_autoInit;

		// The body of a C# catch (Exception ex): non-std exceptions are reported too.
		static void OnCurrentException()
		{
			try
			{
				throw;
			}
			catch (const std::exception& exception)
			{
				Exception.Invoke(exception);
			}
			catch (...)
			{
				Exception.Invoke(std::runtime_error("Unknown exception"));
			}
		}

		static int32_t ConsumeReminders(Tools::Timestamp now)
		{
			int32_t count = 0;
			Tools::Timestamp priority;
			Reminder reminder;
			while (s_reminders.TryPeek(priority, reminder) && reminder.Timestamp <= now)
			{
				// C# TryDequeue: a TryRemoveReminder between the peek and here could pop a reminder not yet due.
				if (s_reminders.TryDequeueIfAtMost(now, priority, reminder))
				{
					try
					{
						count += 1;
						(*reminder.Callback)(reminder.Timestamp);
					}
					catch (...)
					{
						OnCurrentException();
					}
				}
			}
			return count;
		}

		static void RunSimulation()
		{
			s_simulationNow.store(Begin(), std::memory_order_relaxed);

			s_anchorWallTicks = GetWallTicks();
			s_anchorSimTime = s_simulationNow.load(std::memory_order_relaxed);

			while (!IsStopping() && s_simulationNow.load(std::memory_order_relaxed) < End())
			{
				try
				{
					ConsumeReminders(s_simulationNow.load(std::memory_order_relaxed));

					// Determine next stop
					s_interjection.store(End(), std::memory_order_relaxed);
					Tools::Timestamp priority;
					Reminder next;
					if (s_reminders.TryPeek(priority, next))
						s_interjection.store(next.Timestamp, std::memory_order_relaxed);

					Interject.Invoke(s_interjection.load(std::memory_order_relaxed));

					SimulateSpeed();

					s_simulationNow.store(s_interjection.load(std::memory_order_relaxed), std::memory_order_relaxed);
					TickTock.Invoke(s_simulationNow.load(std::memory_order_relaxed));
				}
				catch (...)
				{
					OnCurrentException();
				}
			}

			try
			{
				ConsumeReminders(s_simulationNow.load(std::memory_order_relaxed));
			}
			catch (...)
			{
				OnCurrentException();
			}
			Interject.Invoke(s_interjection.load(std::memory_order_relaxed));

			s_isRunning = false;
		}

		static void SimulateSpeed()
		{
			double simulationSpeed = s_simulationSpeed.load(std::memory_order_relaxed);
			Tools::Timestamp interjection = s_interjection.load(std::memory_order_relaxed);
			if (simulationSpeed < std::numeric_limits<double>::max() && interjection > s_simulationNow.load(std::memory_order_relaxed))
			{
				// Wall time due = sim time since the last speed change (the anchor) / speed.
				Tools::Duration simElapsed = interjection - s_anchorSimTime;
				double targetWallSeconds = simElapsed.GetTotalSeconds() / simulationSpeed;
				int64_t targetWallTicks = s_anchorWallTicks + static_cast<int64_t>(targetWallSeconds * s_wallTicksPerSecond);

				while (true)
				{
					if (IsStopping() || s_isSimulationSpeedChanging)
						break;

					int64_t ticksToWait = targetWallTicks - GetWallTicks();

					if (ticksToWait <= 0)
						break;

					if (ticksToWait > s_wallTicksPerSecond / 1000)
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
					else
						for (int32_t i = 0; i < 10; i++)
							_mm_pause();
				}
			}
		}

		static void RunRealtime()
		{
			while (!IsStopping())
			{
				try
				{
					Tools::Timestamp now = Tools::Timestamp::UtcNow();
					int32_t count = ConsumeReminders(now);
					TickTock.Invoke(now);
					if (count == 0)
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				catch (...)
				{
					OnCurrentException();
				}
			}

			s_isRunning = false;
		}

	public:
		Clock() = delete;

		static inline Event<const std::exception&> Exception;
		static inline Event<Tools::Timestamp> TickTock;
		static inline Event<Tools::Timestamp> Started;
		static inline Event<Tools::Timestamp> Stopped;
		static inline Event<Tools::Timestamp> Interject;

		// Reports an exception caught outside the Clock's own callbacks; Scenario wires these into the AlertManager.
		static void OnException(const std::exception& exception)
		{
			Exception.Invoke(exception);
		}

		static bool IsRunning() { return s_isRunning; }
		static bool IsStopping() { return s_isStopping; }

		static ClockMode Mode() { return s_mode; }

		static void SetMode(ClockMode mode)
		{
			if (IsRunning())
				throw std::logic_error("Can not set mode after Clock has been started.");
			s_mode = mode;
		}

		static Tools::Timestamp Begin() { return s_begin; }

		static void SetBegin(Tools::Timestamp begin)
		{
			if (IsRunning() && Mode() == ClockMode::Simulation)
				throw std::logic_error("Cannot set Begin while simulation is running.");
			s_begin = begin;
			s_simulationNow.store(s_begin, std::memory_order_relaxed);
		}

		static Tools::Timestamp End() { return s_end; }
		static void SetEnd(Tools::Timestamp end) { s_end = end; }

		static int32_t RemindersQueued() { return s_reminders.Count(); }

		static Tools::Timestamp UtcNow()
		{
			return Mode() == ClockMode::Simulation ? s_simulationNow.load(std::memory_order_relaxed) : Tools::Timestamp::UtcNow();
		}

		static void AddReminder(const Reminder& reminder) { s_reminders.Enqueue(reminder.Timestamp, reminder); }

		static void TryRemoveReminder(const Reminder& reminder) { s_reminders.TryRemove(reminder); }

		// Allow external "interjection" of a time to jump to (simulation)
		static void OnInterject(Tools::Timestamp timestamp)
		{
			if (timestamp >= s_simulationNow.load(std::memory_order_relaxed))
				s_interjection.store(Tools::Timestamp::Min(s_interjection.load(std::memory_order_relaxed), timestamp), std::memory_order_relaxed);
		}

		static void Start()
		{
			{
				std::lock_guard<std::mutex> lock(s_lock);
				if (s_isRunning)
					throw std::logic_error("Clock already running.");

				// Before s_isRunning, so a signal handled on this thread never sees a running clock with a stale id.
				s_runningThreadId = std::this_thread::get_id();
				s_isRunning = true;
			}

			std::cout << "Clock::" << (Mode() == ClockMode::Simulation ? "Simulation" : "Realtime") << " started." << std::endl;

			try
			{
				try
				{
					Started.Invoke(UtcNow());

					if (Mode() == ClockMode::Simulation)
						RunSimulation();
					else
						RunRealtime();
				}
				catch (...)
				{
					OnCurrentException();
				}
			}
			catch (...)
			{
				// C# leaves IsRunning set when a throw skips the Run* epilogue; the "Stop Clock" exit action would then wait forever.
				s_isRunning = false;
				Stopped.Invoke(UtcNow());
				throw;
			}
			s_isRunning = false;
			Stopped.Invoke(UtcNow());
		}

		static void Stop() { s_isStopping = true; }

		static double SimulationSpeed() { return s_simulationSpeed.load(std::memory_order_relaxed); }

		static void SetSimulationSpeed(double value)
		{
			s_isSimulationSpeedChanging = true;
			AddReminder(Reminder(Tools::Timestamp::MinValue, [value](Tools::Timestamp)
			{
				s_isSimulationSpeedChanging = false;

				if (std::abs(s_simulationSpeed.load(std::memory_order_relaxed) - value) < 0.001)
					return;

				// Re-anchor at the current instant so the pacing drift restarts from zero under the new speed.
				if (IsRunning() && Mode() == ClockMode::Simulation)
				{
					s_anchorWallTicks = GetWallTicks();
					s_anchorSimTime = s_simulationNow.load(std::memory_order_relaxed);
				}

				s_simulationSpeed.store(value, std::memory_order_relaxed);
			}, "SetSimulationSpeed"));
		}
	};
}
