//BEGIN_FILE HFT/Tools/Latency.hpp
#pragma once

#include <cstdint>
#include <algorithm>
#include <span>
#include <functional>
#include <chrono>
#include <array>
#include "Timestamp.hpp"

namespace Tools
{
	enum class ClientCallStack : int32_t
	{
		ClientRead = 0,
		ClientPublish = 1,
		AlgoExecute = 2,
		AlgoTarget = 3,
		ClientSend = 4,
		ClientCreate = 5,
		ClientAmend = 6,
		ClientValidate = 7,
		ClientWrite = 8,
		Count = 9
	};

    enum class ServerCallStack : int32_t
	{
		ServerReadSocket = 0,
        ServerParseMBP = 1,
		ServerWriteMBP = 2,
		ServerParseMBO = 3,
		ServerWriteMBO = 4,
		ServerParseTrade = 5,
		ServerWriteTrade = 6,
		ServerParseOrderTarget = 7,
		ServerWriteOrderTarget = 8,
		ServerParseOrderState = 9,
        ServerWriteOrderState = 10,
		ServerLog = 11,
		ExchangeToNicMBP = 12,
		ExchangeToNicOrderState = 12,
		ExchangeToExchange = 13,
		NicToSend = 14,
		NicToNic = 15,
		Count = 16
	};

	struct LatencyRecord
	{
		int32_t CallId;
		int64_t StartTicks;
		int64_t ChildrenTicks;
		int64_t TotalTicks;

		[[nodiscard]] Tools::Duration ExclusiveDuration() const noexcept
		{
			return Tools::Duration::FromNanoseconds(std::max<int64_t>(0, TotalTicks - ChildrenTicks));
		}

		[[nodiscard]] Tools::Duration TotalDuration() const noexcept
		{
			return Tools::Duration::FromNanoseconds(TotalTicks);
		}

		[[nodiscard]] Tools::Duration ChildrenDuration() const noexcept
		{
			return Tools::Duration::FromNanoseconds(ChildrenTicks);
		}
	};

	class Latency
	{
	public:
		static inline bool Enabled = true;
		static inline std::function<void(std::span<const LatencyRecord>)> OnFlush = nullptr;
		static constexpr int32_t MaxRecords = 1024;
		
		static constexpr double NanosPerTick = 1.0;

	private:
		static inline thread_local std::array<LatencyRecord, MaxRecords> s_records{};
		static inline thread_local int32_t s_count = 0;
		static inline thread_local int32_t s_stackDepth = 0;
		static inline thread_local int32_t s_currentParentIndex = -1;

		int32_t _recordIndex;
		int32_t _parentIndex;
		int64_t _startTicks;
		bool _isCanceled;

		static void FlushAndClear()
		{
			if (OnFlush && s_count > 0)
			{
				OnFlush(std::span<const LatencyRecord>(s_records.data(), static_cast<size_t>(s_count)));
			}

			s_count = 0;
			s_currentParentIndex = -1;
		}

		// Write a single record straight to OnFlush as its own span, independent of the thread-local
		// buffer and its parent/child accounting. Used by RecordDuration for standalone timings.
		static void Flush(const LatencyRecord& record)
		{
			if (OnFlush)
			{
				OnFlush(std::span<const LatencyRecord>(&record, 1));
			}
		}

	public:
		Latency(const Latency&) = delete;
		Latency& operator=(const Latency&) = delete;
		Latency(Latency&&) = delete;
		Latency& operator=(Latency&&) = delete;

		explicit Latency(int32_t callId) : _recordIndex(-1), _parentIndex(s_currentParentIndex), _startTicks(0), _isCanceled(false)
		{
			if (!Enabled)
			{
				return;
			}

			_startTicks = GetTimestamp();

			_recordIndex = s_count++;
			s_stackDepth++;
			s_currentParentIndex = _recordIndex;

			if (_recordIndex < MaxRecords)
			{
				LatencyRecord& record = s_records[static_cast<size_t>(_recordIndex)];
				record.CallId = callId;
				record.StartTicks = _startTicks;
				record.ChildrenTicks = 0;
				record.TotalTicks = 0;
			}
		}

        static inline int64_t GetTimestamp() noexcept
		{
			return std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()
			).count();
		}

		// Record a pre-measured duration as an independent latency point, for timings computed from
		// timestamps (e.g. nic-to-nic, exchange-to-exchange) rather than a steady_clock scope. It
		// bypasses the thread-local buffer entirely: builds one record and flushes it straight to
		// OnFlush, ignoring every other record and any surrounding RAII scope (neither touches the
		// other). ChildrenTicks stays 0 so ExclusiveDuration() == the full duration; cross-clock skew
		// that goes negative clamps to 0 there.
		static void Record(int32_t callId, Tools::Duration duration) noexcept
		{
			if (!Enabled)
			{
				return;
			}

			LatencyRecord record{};
			record.CallId = callId;
			record.StartTicks = GetTimestamp();
			record.ChildrenTicks = 0;
			record.TotalTicks = duration.TotalNanoseconds;
			Flush(record);
		}

		void Cancel() noexcept
		{
			_isCanceled = true;
		}

		~Latency()
		{
			if (!Enabled)
			{
				return;
			}

			s_stackDepth--;

			if (_recordIndex == s_currentParentIndex)
			{
				s_currentParentIndex = _parentIndex;
			}

			if (_isCanceled)
			{
				if (_recordIndex == s_count - 1)
				{
					s_count--;
				}

				if (s_stackDepth == 0)
				{
					FlushAndClear();
				}
				
				return;
			}

			int64_t endTicks = GetTimestamp();
			int64_t totalTicks = endTicks - _startTicks;

			if (_recordIndex < MaxRecords)
			{
				s_records[static_cast<size_t>(_recordIndex)].TotalTicks = totalTicks;

				if (_parentIndex >= 0 && _parentIndex < MaxRecords)
				{
					s_records[static_cast<size_t>(_parentIndex)].ChildrenTicks += totalTicks;
				}
			}

			if (s_stackDepth == 0)
			{
				FlushAndClear();
			}
		}
	};
}
//END_FILE HFT/Tools/Latency.hpp