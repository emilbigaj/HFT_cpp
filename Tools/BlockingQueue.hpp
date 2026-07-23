#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include <utility>

namespace Tools
{
	// Multi-producer / single-consumer blocking queue, modelled on C#'s BlockingCollection<T>:
	// producers Add() until CompleteAdding(), then the consumer drains to empty. Cold path only.
	template <typename T>
	class BlockingQueue
	{
	private:
		std::queue<T> _queue;
		std::mutex _mutex;
		std::condition_variable _cv;
		bool _completed = false;

	public:
		// Returns false if adding has been completed (mirrors BlockingCollection throwing on Add).
		bool Add(T item)
		{
			{
				std::lock_guard<std::mutex> lock(_mutex);
				if (_completed)
					return false;
				_queue.push(std::move(item));
			}
			_cv.notify_one();
			return true;
		}

		void CompleteAdding()
		{
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_completed = true;
			}
			_cv.notify_all();
		}

		// Blocking take: waits for an item. Returns false once completed AND drained
		// (mirrors GetConsumingEnumerable() terminating).
		bool Take(T& out)
		{
			std::unique_lock<std::mutex> lock(_mutex);
			_cv.wait(lock, [this] { return !_queue.empty() || _completed; });

			if (_queue.empty())
				return false;

			out = std::move(_queue.front());
			_queue.pop();
			return true;
		}

		// Non-blocking take: returns immediately, true if an item was popped.
		bool TryTake(T& out)
		{
			std::lock_guard<std::mutex> lock(_mutex);

			if (_queue.empty())
				return false;

			out = std::move(_queue.front());
			_queue.pop();
			return true;
		}
	};
}
