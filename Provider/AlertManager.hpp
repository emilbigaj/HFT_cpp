#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <source_location>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

#include "BlockingQueue.hpp"
#include "Json.hpp"
#include "LowLatency.hpp"
#include "Tools.hpp"

#include "Context.hpp"
#include "Instrument.hpp" // Data::Header<T>
#include "Order.hpp"      // Execution::OrderRejected
#include "Socket.hpp"

namespace Provider
{
	enum class AlertType : uint8_t
	{
		Exception = 0,
		OrderRejected = 1,
		ExchangeDead = 2,   // exchange disconnect
		InstrumentDead = 3, // no new data for instrument
		OrderDead = 4,      // order not acked within 1 second
	};

	struct Alert
	{
		Data::Header<AlertType> Header;

		// Typed payload (C#'s boxed `object?`). std::monostate = no object (message-only).
		// Each non-monostate alternative must be POD so its bytes go on the wire verbatim.
		std::variant<std::monostate, Execution::OrderRejected> Object;

		std::string Message;

		// Held outside the wire/JSON layout (see glaze below + WriteToSocket): the producer
		// captures the live exception via std::current_exception() — a refcount bump, no copy or
		// slicing — and the consumer rethrows it later to materialize Message. Pays for demangle,
		// what(), and the nested chain on the background thread instead of the alert call site.
		// Null/default on non-Exception alerts.
		std::exception_ptr Exception;
		std::source_location Location;

		Alert() = default;

		// Message-only (e.g. pre-resolved text alerts).
		Alert(AlertType type, std::string message)
			: Header(type), Object(std::monostate{}), Message(std::move(message)) {}

		// Object + message.
		template <typename T> requires Tools::PlainOldData<T>
		Alert(AlertType type, const T& object, std::string message)
			: Header(type), Object(object), Message(std::move(message)) {}

		// Deferred-exception alert: rich Message built by the consumer from Exception + Location.
		Alert(std::exception_ptr exception, std::source_location location)
			: Header(AlertType::Exception), Object(std::monostate{}),
			  Exception(std::move(exception)), Location(location) {}

		// ToString()/JSON includes Object (glaze writes the active alternative; monostate -> null).
		// On the wire it's the raw struct bytes instead — the two representations are independent.
		struct glaze
		{
			using T = Alert;
			static constexpr auto value = glz::object(
				"Header", &T::Header,
				"Object", &T::Object,
				"Message", &T::Message
			);
		};

		std::string ToString() const
		{
			return Tools::Json::Serialize(*this);
		}
	};

	class AlertManager
	{
	public:
		const std::string MachineName;
		const Provider::Context& Context;

	private:
		std::unique_ptr<Socket::ClientSocket> _logger;
		Tools::BlockingQueue<Alert> _queue;
		std::thread _thread;
		std::array<uint8_t, 64 * 1024> _buffer{};
		std::atomic<bool> _disposed{false};

	public:
		explicit AlertManager(Provider::Context& context)
			: MachineName(Tools::MachineName()), Context(context)
		{
			_logger = std::make_unique<Socket::ClientSocket>(
				Context.DirectoryPath.string() + ".alert",
				Context.LoggingServerName,
				std::vector<int32_t>{Socket::SocketChannel::ExecutionChannelLength},
				std::vector<int32_t>{Socket::SocketChannel::AdminChannelLength});
			_logger->Connect();

			// NOTE: C#'s TaskScheduler.UnobservedTaskException hook has no analogue here (no task
			// scheduler); callers invoke OnException()/OnOrderRejected() directly.
			_thread = Tools::LowLatency::StartBackgroundThread("AlertManager", [this]() { ConsumeLoop(); });
		}

		~AlertManager()
		{
			Dispose();
		}

		AlertManager(const AlertManager&) = delete;
		AlertManager& operator=(const AlertManager&) = delete;
		AlertManager(AlertManager&&) = delete;
		AlertManager& operator=(AlertManager&&) = delete;

		// Must be called from inside a catch handler: current_exception() returns null otherwise,
		// and we rely on it to preserve the dynamic type for the consumer-side formatter.
		void OnException(std::exception, std::source_location location = std::source_location::current())
		{
			if (_disposed.load(std::memory_order_acquire))
				return;

			// Refcount bump only — demangle / what() / nested-chain walk happen in ConsumeLoop.
			_queue.Add(Alert(std::current_exception(), location));
		}

		void OnOrderRejected(const Execution::OrderRejected& orderRejected, const std::string& message)
		{
			if (_disposed.load(std::memory_order_acquire))
				return;

			_queue.Add(Alert(AlertType::OrderRejected, orderRejected, message));
		}

		void Dispose()
		{
			if (_disposed.exchange(true))
				return;

			_queue.CompleteAdding();

			if (_thread.joinable())
				_thread.join();

			if (_logger)
				_logger->Dispose();
		}

	private:
		// Rich text for a captured exception: "<file>:<line> in <function>\n<Type>: <what>"
		// with "\nCaused by: <Type>: <what>" appended for each level of std::throw_with_nested.
		static std::string FormatException(std::exception_ptr exception, const std::source_location& location)
		{
			std::string out;
			out.reserve(256);
			out += location.file_name();
			out += ':';
			out += std::to_string(location.line());
			out += " in ";
			out += location.function_name();
			out += '\n';
			AppendException(out, std::move(exception));
			return out;
		}

		static void AppendException(std::string& out, std::exception_ptr exception)
		{
			try
			{
				std::rethrow_exception(std::move(exception));
			}
			catch (const std::exception& e)
			{
				out += Tools::GetTypeName(typeid(e));
				out += ": ";
				out += e.what();

				try
				{
					std::rethrow_if_nested(e);
				}
				catch (...)
				{
					out += "\nCaused by: ";
					AppendException(out, std::current_exception());
				}
			}
			catch (...)
			{
				out += "<non-std exception>";
			}
		}

		void ConsumeLoop()
		{
			try
			{
				Alert alert;
				while (_queue.Take(alert))
				{
					// Deferred from OnException — pay demangle/what()/nested walk here, not on the
					// caller's thread. Skipped if Exception is null (non-Exception alerts).
					if (alert.Exception)
						alert.Message = FormatException(alert.Exception, alert.Location);

					try
					{
						WriteToSocket(alert);
					}
					catch (const std::exception& exception)
					{
						std::cout << "AlertManager.WriteToSocket() failed. Payload:\n"
								  << alert.Message << "\nCause:\n" << exception.what() << std::endl;
					}
				}
			}
			catch (const std::exception& exception)
			{
				std::cout << "AlertManager.ConsumeLoop() crashed: " << exception.what() << std::endl;
			}
		}

		// Wire layout: [Header] [Object bytes (e.g. OrderRejected struct)] [ASCII message].
		void WriteToSocket(const Alert& alert)
		{
			constexpr int32_t headerSize = static_cast<int32_t>(sizeof(Data::Header<AlertType>));
			const int32_t bufferSize = static_cast<int32_t>(_buffer.size());
			int32_t pos = 0;

			std::memcpy(_buffer.data() + pos, &alert.Header, static_cast<size_t>(headerSize));
			pos += headerSize;

			// C#'s `switch (alert.Header.Type)` -> type-safe visit; monostate writes nothing.
			std::visit([&](const auto& object)
			{
				using O = std::decay_t<decltype(object)>;
				if constexpr (!std::is_same_v<O, std::monostate>)
				{
					const int32_t bytes = std::min(static_cast<int32_t>(sizeof(O)), bufferSize - pos);
					std::memcpy(_buffer.data() + pos, &object, static_cast<size_t>(bytes));
					pos += bytes;
				}
			}, alert.Object);

			if (!alert.Message.empty())
			{
				const int32_t bytes = std::min(static_cast<int32_t>(alert.Message.size()), bufferSize - pos);
				std::memcpy(_buffer.data() + pos, alert.Message.data(), static_cast<size_t>(bytes));
				pos += bytes;
			}

			_logger->Write(std::span<const uint8_t>(_buffer.data(), static_cast<size_t>(pos)));
		}
	};
}
