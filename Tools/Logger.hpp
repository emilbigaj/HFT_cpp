#pragma once

// COLD PROCESS LOGGER
// NOT ALLOWED ON THE HOT PATH

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include <glaze/glaze.hpp>

#include "Application.hpp"
#include "BlockingQueue.hpp"
#include "Json.hpp"
#include "LowLatency.hpp"
#include "Timestamp.hpp"

namespace Tools
{
	// Mirror of the C# LogEntry. The payload objects are captured as deferred
	// serializer closures so the JSON formatting happens on the background writer
	// thread (entry.ToString()), not on the thread that called Log().
	//
	// NOTE: C# stored boxed object references and the GC kept them alive until the
	// worker serialized them. C++ has no GC, so each argument is copied into its
	// serializer closure at Log() time instead of held by reference.
	struct LogEntry
	{
		Tools::Timestamp Timestamp;
		std::string Thread;
		std::string Source;
		std::vector<std::function<std::string()>> Objects;

		// Scratch filled by ToString() on the writer thread: the deferred serializers are
		// invoked to produce raw JSON fragments. glaze emits glz::raw_json verbatim, matching
		// the C# converter's writer.WriteRawValue(Json.Serialize(obj)). 'Objects' (the closures)
		// is intentionally absent from the meta, so glaze never inspects the std::function vector.
		mutable std::vector<glz::raw_json> JsonObjects;

		struct glaze
		{
			using T = LogEntry;
			static constexpr auto value = glz::object(
				"Timestamp", &T::Timestamp,
				"Thread", &T::Thread,
				"Source", &T::Source,
				"Objects", &T::JsonObjects
			);
		};

		// Pretty (multi-line) JSON, same shape as LogEntryJsonConverter. Runs on the writer
		// thread, so per-object serialization stays off the thread that called Log().
		std::string ToString() const
		{
			JsonObjects.clear();
			JsonObjects.reserve(Objects.size());

			for (const std::function<std::string()>& serializer : Objects)
				JsonObjects.emplace_back(glz::raw_json{serializer()});

			return Tools::Json::Serialize(*this);
		}
	};

	class Logger
	{
	public:
		bool ToConsole = false;
		bool ToFile = true;

		std::function<void(const std::exception&)> Exception;

		const std::filesystem::path DirectoryPath;

	private:
		Tools::Timestamp _date = Tools::Timestamp::MinValue;
		BlockingQueue<LogEntry> _logQueue;
		std::thread _workerThread;
		bool _started = false;

		std::atomic<bool> _finished{false}; // signalled when WriteLoop() exits
		std::atomic<int> _disposeCASLock{0};

	public:
		explicit Logger(const std::filesystem::path& directoryPath) : DirectoryPath(directoryPath)
		{
			std::filesystem::create_directories(DirectoryPath);

			// int::min priority => disposed last (Application sorts descending by priority).
			Tools::Application::AddExitAction("Dispose Logger " + DirectoryPath.string(),
				std::numeric_limits<int>::min(), [this]() { Dispose(); });

			Connect();
		}

		~Logger()
		{
			Dispose();
		}

		Logger(const Logger&) = delete;
		Logger& operator=(const Logger&) = delete;
		Logger(Logger&&) = delete;
		Logger& operator=(Logger&&) = delete;

		[[nodiscard]] Tools::Timestamp Date() const
		{
			return _date;
		}

		template <typename... Args>
		void Log(const std::string& source, Args&&... args)
		{
			LogEntry entry;
			entry.Timestamp = Tools::Timestamp::UtcNow();
			entry.Thread = GetCurrentThreadLabel();
			entry.Source = source;
			entry.Objects.reserve(sizeof...(Args));
			(entry.Objects.emplace_back(MakeSerializer(std::forward<Args>(args))), ...);

			if (!_logQueue.Add(std::move(entry)))
			{
				// Queue closed (CompleteAdding called). Mirror C#'s catch-and-report.
				std::cout << "Logger::Log() Exception, failed to log from source: " << source << std::endl;
			}
		}

		[[nodiscard]] std::filesystem::path GetFilePath(Tools::Timestamp timestamp) const
		{
			return DirectoryPath / (timestamp.ToDateString() + ".log");
		}

		void Dispose()
		{
			if (_disposeCASLock.exchange(1) == 1)
				return;

			_logQueue.CompleteAdding();

			if (_started)
			{
				while (!_finished.load(std::memory_order_acquire))
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1000));

					if (!_finished.load(std::memory_order_acquire))
						std::cout << "Logger::" << DirectoryPath.string()
								  << " Waiting for logger to close and dispose." << std::endl;
				}

				// Unconditional join: block until the worker has fully exited (no detach -> no UB).
				if (_workerThread.joinable())
					_workerThread.join();
			}
		}

	private:
		// Captures a copy of the argument and returns a closure that serializes it
		// lazily on the writer thread. Character arrays / pointers are normalised to
		// std::string so we own the bytes; everything else must be glaze-serializable.
		template <typename T>
		static std::function<std::string()> MakeSerializer(T&& value)
		{
			using V = std::decay_t<T>;

			if constexpr (std::is_same_v<V, std::string>)
			{
				return [v = std::string(std::forward<T>(value))]() { return Tools::Json::SerializeToLine(v); };
			}
			else if constexpr (std::is_convertible_v<V, std::string_view>)
			{
				// string literals / const char*
				return [v = std::string(std::string_view(value))]() { return Tools::Json::SerializeToLine(v); };
			}
			else
			{
				return [v = V(std::forward<T>(value))]() { return Tools::Json::SerializeToLine(v); };
			}
		}

		static std::string GetCurrentThreadLabel()
		{
			std::string name = Tools::LowLatency::GetCurrentThreadName();
			if (!name.empty())
				return name;

			std::ostringstream oss;
			oss << "Thread-" << std::this_thread::get_id();
			return oss.str();
		}

		void Connect()
		{
			if (_started)
				return;

			_started = true;
			_workerThread = Tools::LowLatency::StartBackgroundThread(
				"Logger " + DirectoryPath.string(), [this]() { WriteLoop(); });
		}

		void WriteLoop()
		{
			std::unique_ptr<std::ofstream> stream;

			auto closeFile = [&stream]()
			{
				if (stream)
				{
					try { stream->flush(); } catch (...) {}
					stream.reset();
				}
			};

			LogEntry entry;

			try
			{
				while (_logQueue.Take(entry))
				{
					try
					{
						if (!stream || entry.Timestamp.Date() != _date)
						{
							_date = entry.Timestamp.Date();
							closeFile();

							std::filesystem::create_directories(DirectoryPath);

							stream = std::make_unique<std::ofstream>(
								GetFilePath(_date), std::ios::out | std::ios::app | std::ios::binary);

							if (!stream->is_open())
								throw std::runtime_error("Logger: failed to open log file " + GetFilePath(_date).string());
						}

						std::string line = entry.ToString();

						if (ToFile && stream)
						{
							*stream << line << '\n';
							stream->flush(); // AutoFlush = true
						}

						if (ToConsole)
							std::cout << line << std::endl;
					}
					catch (const std::exception& ex)
					{
						std::cout << "LOGGER ERROR: " << ex.what() << std::endl;

						if (Exception)
							Exception(ex);
					}
				}
			}
			catch (...)
			{
				// Swallow: the cleanup below still closes the file and signals completion.
			}

			closeFile();
			_finished.store(true, std::memory_order_release);
		}
	};
}
