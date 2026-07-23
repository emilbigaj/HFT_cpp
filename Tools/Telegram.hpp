#pragma once

// TELEGRAM BOT CLIENT
// Background sender: batches queued messages and rate-limits delivery. Cold path only.
// https://core.telegram.org/bots/api#available-methods

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include <curl/curl.h>

#include "BlockingQueue.hpp"
#include "Json.hpp"
#include "Logger.hpp"
#include "LowLatency.hpp"

namespace Tools
{
	// Telegram secrets — kept OUT of source control. Loaded once at first use from a machine-local
	// file (Telegram::ConfigPath: /mnt/S/Telegram.json). A template lives at Tools/Telegram.example.json.
	struct TelegramConfig
	{
		std::string AlertChatId;
		std::string NotificationChatId;
		std::string ApiToken;
	};

	class Telegram
	{
	public:
		static constexpr const char* ConfigPath = "/mnt/S/Telegram.json";

		static const std::string& AlertChatId() { return Config().AlertChatId; }
		static const std::string& NotificationChatId() { return Config().NotificationChatId; }

		// Optional sink for delivery failures (set by the owner). Mirrors C#'s public Logger?.
		Tools::Logger* Logger = nullptr;

	private:
		static constexpr const char* ApiBase = "https://api.telegram.org/";
		static constexpr int32_t MaxMessageLength = 4000;
		static constexpr std::chrono::seconds SendInterval{3};
		static inline const std::string Separator = "\n\n";

		const std::string _chatId;
		const std::string _urlPrefix; // "<base>bot<token>/sendMessage?chat_id=<chatId>&text="
		CURL* _curl = nullptr;
		BlockingQueue<std::string> _queue;
		std::thread _thread;
		std::atomic<bool> _disposed{false};

	public:
		explicit Telegram(const std::string& chatId)
			: _chatId(chatId),
			  _urlPrefix(std::string(ApiBase) + "bot" + Config().ApiToken + "/sendMessage?chat_id=" + chatId + "&text=")
		{
			EnsureCurlGlobalInit();

			_curl = curl_easy_init();
			if (_curl == nullptr)
				throw std::runtime_error("Telegram: curl_easy_init failed");

			// Stable options; only the URL changes per request.
			curl_easy_setopt(_curl, CURLOPT_WRITEFUNCTION, &DiscardBody);
			curl_easy_setopt(_curl, CURLOPT_NOSIGNAL, 1L);
			curl_easy_setopt(_curl, CURLOPT_TIMEOUT, 10L);

			_thread = Tools::LowLatency::StartBackgroundThread("Telegram", [this]() { ConsumeLoop(); });
		}

		~Telegram()
		{
			Dispose();
		}

		Telegram(const Telegram&) = delete;
		Telegram& operator=(const Telegram&) = delete;
		Telegram(Telegram&&) = delete;
		Telegram& operator=(Telegram&&) = delete;

		// Join the arguments with newlines and enqueue for delivery (C#'s params object[] overload).
		template <typename... Args>
		void Send(Args&&... args)
		{
			if (_disposed.load(std::memory_order_acquire))
				return;

			_queue.Add(Join(std::forward<Args>(args)...));
		}

		void Dispose()
		{
			if (_disposed.exchange(true))
				return;

			_queue.CompleteAdding();

			if (_thread.joinable())
				_thread.join();

			if (_curl != nullptr)
			{
				curl_easy_cleanup(_curl);
				_curl = nullptr;
			}
		}

	private:
		// Loaded once, on first use. A missing or malformed file leaves every field empty: chat ids
		// resolve to "" and requests fail per-message through the normal Logger path, so a machine
		// without the secrets file still starts.
		static const TelegramConfig& Config()
		{
			static const TelegramConfig config = LoadConfig();
			return config;
		}

		static TelegramConfig LoadConfig()
		{
			try
			{
				std::ifstream file(ConfigPath, std::ios::binary);
				if (!file)
					throw std::runtime_error("file not found");

				std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
				return Tools::Json::Deserialize<TelegramConfig>(json);
			}
			catch (const std::exception& exception)
			{
				std::cout << "Telegram: could not load config from " << ConfigPath << " (" << exception.what()
						  << "); alerts disabled." << std::endl;
				return TelegramConfig{};
			}
		}

		static void EnsureCurlGlobalInit()
		{
			static const bool ok = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
			if (!ok)
				throw std::runtime_error("Telegram: curl_global_init failed");
		}

		template <typename... Args>
		static std::string Join(Args&&... args)
		{
			std::ostringstream oss;
			bool first = true;

			auto append = [&](auto&& value)
			{
				if (!first)
					oss << '\n';
				first = false;
				oss << ToText(std::forward<decltype(value)>(value));
			};

			(append(std::forward<Args>(args)), ...);
			return oss.str();
		}

		template <typename T>
		static std::string ToText(T&& value)
		{
			using V = std::decay_t<T>;

			if constexpr (std::is_same_v<V, std::string>)
				return std::forward<T>(value);
			else if constexpr (std::is_convertible_v<V, std::string_view>)
				return std::string(std::string_view(value));
			else
				return Tools::Json::SerializeToLine(value);
		}

		void ConsumeLoop()
		{
			std::string buffer;
			buffer.reserve(MaxMessageLength);

			try
			{
				std::string first;
				while (_queue.Take(first))
				{
					AppendOrChunk(buffer, first);

					std::string next;
					while (_queue.TryTake(next))
						AppendOrChunk(buffer, next);

					Flush(buffer);
				}
			}
			catch (const std::exception& exception)
			{
				try { if (Logger) Logger->Log("Telegram", std::string(exception.what())); }
				catch (...) { std::cout << exception.what() << std::endl; }
			}
		}

		void AppendOrChunk(std::string& builder, const std::string& item)
		{
			// Fits in one message: flush pending if it won't co-pack, then append whole.
			if (static_cast<int32_t>(item.size()) <= MaxMessageLength)
			{
				const int32_t sepLen = builder.empty() ? 0 : static_cast<int32_t>(Separator.size());
				if (static_cast<int32_t>(builder.size()) + sepLen + static_cast<int32_t>(item.size()) > MaxMessageLength)
					Flush(builder);

				if (!builder.empty())
					builder += Separator;
				builder += item;
				return;
			}

			// Item exceeds a single message: flush pending, then split across N full-sized messages.
			Flush(builder);
			size_t idx = 0;
			while (idx < item.size())
			{
				const size_t take = std::min(item.size() - idx, static_cast<size_t>(MaxMessageLength));
				builder.append(item, idx, take);
				idx += take;
				if (idx < item.size())
					Flush(builder);
			}
		}

		void Flush(std::string& builder)
		{
			if (builder.empty())
				return;

			std::string text = std::move(builder);
			builder.clear();

			try
			{
				Get(text);
			}
			catch (const std::exception& exception)
			{
				try { if (Logger) Logger->Log("Telegram", text, std::string(exception.what())); }
				catch (...) { std::cout << exception.what() << std::endl; }
			}

			std::this_thread::sleep_for(SendInterval);
		}

		void Get(const std::string& text)
		{
			char* escaped = curl_easy_escape(_curl, text.c_str(), static_cast<int>(text.size()));
			if (escaped == nullptr)
				throw std::runtime_error("Telegram: curl_easy_escape failed");

			std::string url = _urlPrefix + escaped;
			curl_free(escaped);

			curl_easy_setopt(_curl, CURLOPT_URL, url.c_str());

			const CURLcode code = curl_easy_perform(_curl);
			if (code != CURLE_OK)
				throw std::runtime_error(std::string("Telegram: request failed: ") + curl_easy_strerror(code));

			long status = 0;
			curl_easy_getinfo(_curl, CURLINFO_RESPONSE_CODE, &status);
			if (status < 200 || status >= 300)
				throw std::runtime_error("Telegram: HTTP status " + std::to_string(status));
		}

		static size_t DiscardBody(char*, size_t size, size_t nmemb, void*)
		{
			return size * nmemb;
		}
	};
}
