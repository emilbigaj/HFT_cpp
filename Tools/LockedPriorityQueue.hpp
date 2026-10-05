#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace Tools
{
	// Min-heap priority queue; equal priorities dequeue in enqueue order. One mutex guards every
	// operation (the C# seqlock + ArrayPool is not needed off the hot path).
	template <typename TPriority, typename TValue>
	class LockedPriorityQueue final
	{
	private:
		struct Entry
		{
			TPriority Priority;
			TValue Value;
			uint64_t EnqueueOrder;
		};

		mutable std::mutex _mutex;
		std::vector<Entry> _entries;
		uint64_t _orderCounter = 0;
		std::atomic<int32_t> _count{ 0 };

		int Compare(size_t i, size_t j) const
		{
			const Entry& a = _entries[i];
			const Entry& b = _entries[j];
			if (a.Priority < b.Priority)
				return -1;
			if (b.Priority < a.Priority)
				return 1;
			// earlier enqueue wins
			return a.EnqueueOrder < b.EnqueueOrder ? -1 : (a.EnqueueOrder > b.EnqueueOrder ? 1 : 0);
		}

		void BubbleUp(size_t index)
		{
			while (index > 0)
			{
				size_t parent = (index - 1) >> 1;
				if (Compare(index, parent) >= 0)
					break;
				std::swap(_entries[index], _entries[parent]);
				index = parent;
			}
		}

		void BubbleDown(size_t index)
		{
			size_t count = _entries.size();
			while (true)
			{
				size_t left = (index << 1) + 1;
				if (left >= count)
					break;

				size_t right = left + 1;
				size_t smallest = (right < count && Compare(right, left) < 0) ? right : left;

				if (Compare(index, smallest) <= 0)
					break;
				std::swap(_entries[index], _entries[smallest]);
				index = smallest;
			}
		}

		void RemoveAt(size_t index)
		{
			size_t last = _entries.size() - 1;

			if (index == last)
			{
				_entries.pop_back();
				return;
			}

			_entries[index] = std::move(_entries[last]);
			_entries.pop_back();

			if (index > 0 && Compare(index, (index - 1) >> 1) < 0)
				BubbleUp(index);
			else
				BubbleDown(index);
		}

		void UpdateCount()
		{
			_count.store(static_cast<int32_t>(_entries.size()), std::memory_order_release);
		}

	public:
		LockedPriorityQueue() : LockedPriorityQueue(16) {}

		explicit LockedPriorityQueue(int32_t initialCapacity)
		{
			_entries.reserve(static_cast<size_t>(initialCapacity < 1 ? 1 : initialCapacity));
		}

		LockedPriorityQueue(const LockedPriorityQueue&) = delete;
		LockedPriorityQueue& operator=(const LockedPriorityQueue&) = delete;

		int32_t Count() const
		{
			return _count.load(std::memory_order_acquire);
		}

		void Enqueue(TPriority priority, TValue value)
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_entries.push_back(Entry{ std::move(priority), std::move(value), _orderCounter++ });
			BubbleUp(_entries.size() - 1);
			UpdateCount();
		}

		bool TryDequeue(TPriority& priority, TValue& value)
		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (_entries.empty())
			{
				priority = TPriority{};
				value = TValue{};
				return false;
			}

			priority = std::move(_entries[0].Priority);
			value = std::move(_entries[0].Value);
			RemoveAt(0);
			UpdateCount();
			return true;
		}

		// Peek and dequeue under one lock: a concurrent TryRemove of the head can not make it pop a later element.
		bool TryDequeueIfAtMost(const TPriority& maxPriority, TPriority& priority, TValue& value)
		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (_entries.empty() || maxPriority < _entries[0].Priority)
			{
				priority = TPriority{};
				value = TValue{};
				return false;
			}

			priority = std::move(_entries[0].Priority);
			value = std::move(_entries[0].Value);
			RemoveAt(0);
			UpdateCount();
			return true;
		}

		bool TryPeek(TPriority& priority, TValue& value) const
		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (_entries.empty())
			{
				priority = TPriority{};
				value = TValue{};
				return false;
			}

			priority = _entries[0].Priority;
			value = _entries[0].Value;
			return true;
		}

		// Removes the first stored element equal to value (heap-array order, as C#).
		bool TryRemove(const TValue& value)
		{
			std::lock_guard<std::mutex> lock(_mutex);
			for (size_t i = 0; i < _entries.size(); i++)
			{
				if (_entries[i].Value == value)
				{
					RemoveAt(i);
					UpdateCount();
					return true;
				}
			}
			return false;
		}

		void Clear()
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_entries.clear();
			_orderCounter = 0;
			UpdateCount();
		}
	};
}
