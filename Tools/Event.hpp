#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace Tools
{
	// A C# event: any number of handlers, invoked in subscription order. Invoke runs on a snapshot,
	// so a handler may subscribe or unsubscribe without disturbing the invocation in progress.
	// std::function has no equality, so += hands back the handle that -= takes (C# -= takes the delegate).
	template <typename... Args>
	class Event final
	{
	public:
		using Handle = uint64_t;

	private:
		using Handlers = std::vector<std::pair<Handle, std::function<void(Args...)>>>;

		mutable std::mutex _mutex;
		std::shared_ptr<const Handlers> _handlers = std::make_shared<const Handlers>();
		Handle _nextHandle = 1;

	public:
		// Returns 0 for an empty handler, which is never subscribed.
		Handle operator+=(std::function<void(Args...)> handler)
		{
			if (!handler)
				return 0;

			std::lock_guard<std::mutex> lock(_mutex);
			auto handlers = std::make_shared<Handlers>(*_handlers);
			Handle handle = _nextHandle++;
			handlers->emplace_back(handle, std::move(handler));
			_handlers = std::move(handlers);
			return handle;
		}

		void operator-=(Handle handle)
		{
			std::lock_guard<std::mutex> lock(_mutex);
			auto handlers = std::make_shared<Handlers>(*_handlers);
			std::erase_if(*handlers, [handle](const auto& entry) { return entry.first == handle; });
			_handlers = std::move(handlers);
		}

		void Invoke(Args... args) const
		{
			std::shared_ptr<const Handlers> handlers;
			{
				std::lock_guard<std::mutex> lock(_mutex);
				handlers = _handlers;
			}
			for (const auto& [handle, handler] : *handlers)
				handler(args...);
		}
	};
}
