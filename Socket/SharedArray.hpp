#pragma once

#include "Tools.hpp"
#include "Protocol.hpp"
#include "SharedMemory.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace Socket
{
	template <typename T> requires Tools::PlainOldData<T>
	class SharedArrayEntry final
	{
	private:
		Protocol::Header64* _headerPtr;
		Tools::Access _access;
		uint64_t _seq;

	public:
		inline uint8_t* GetEntryPtr() const
		{
			return reinterpret_cast<uint8_t*>(_headerPtr);
		}

		SharedArrayEntry(uint8_t* entryPtr, Tools::Access access) : _headerPtr(reinterpret_cast<Protocol::Header64*>(entryPtr)), _access(access), _seq(0) {}

		inline bool IsEmpty() const
		{
			return Protocol::ReadSequence(_headerPtr) == 0;
		}

		inline T* GetPtr() const
		{
			return Protocol::GetValuePointer<T>(GetEntryPtr());
		}

		template <typename TCast> requires Tools::PlainOldData<TCast>
		inline TCast* GetPtr() const
		{
			int32_t castSize = static_cast<int32_t>(sizeof(TCast));
			int32_t storedSize = static_cast<int32_t>(sizeof(T));

			if (castSize > storedSize)
				throw std::invalid_argument("Invalid cast.");

			return Protocol::GetValuePointer<TCast>(GetEntryPtr());
		}

		inline T& GetRef() const
		{
			if (_access == Tools::Access::Read)
				throw std::runtime_error("Readonly");

			return *GetPtr();
		}

		inline const T& GetReadonlyRef() const
		{
			return *GetPtr();
		}

		template <typename TCast> requires Tools::PlainOldData<TCast>
		inline TCast& GetRef() const
		{
			if (_access == Tools::Access::Read)
				throw std::runtime_error("Readonly");

			return *GetPtr<TCast>();
		}

		template <typename TCast> requires Tools::PlainOldData<TCast>
		inline const TCast& GetReadonlyRef() const
		{
			return *GetPtr<TCast>();
		}

		inline ReadStatus TryRead(T& value)
		{
			ReadStatus readStatus = Protocol::TryRead(_headerPtr, value, _seq);
			return readStatus;
		}

		inline T Read()
		{
			// 1. Delegate to lock-free memory read
			T value;
			ReadStatus readStatus = Protocol::TryRead(_headerPtr, value, _seq);

			if (readStatus == ReadStatus::Empty)
				// 2. Throw if Slot was Empty
				throw std::runtime_error("Slot is empty.");

			return value;
		}

		inline void AcquireLock()
		{
			Protocol::AcquireLock(_headerPtr);
		}

		inline void ReleaseLock()
		{
			Protocol::ReleaseLock(_headerPtr);
		}

		inline void Write(const T& value)
		{
			// 1. Ensure caller has permission to write
			if (_access == Tools::Access::Read)
				throw std::runtime_error("Entry is read-only.");

			// 2. Execute memory block write 
			int32_t dstLen = Protocol::HeaderLength + static_cast<int32_t>(sizeof(T));
			Protocol::Write(value, _headerPtr, dstLen);
		}

		// Recovery write for a slot whose client process is confirmed dead (Server::CancelAllOrders):
		// bypasses the Read guard (mapping is RW, Read is only a tag) and re-bases the seqlock.
		inline void RecoveryWrite(const T& value)
		{
			int32_t dstLen = Protocol::HeaderLength + static_cast<int32_t>(sizeof(T));
			Protocol::RecoveryWrite(value, _headerPtr, dstLen);
		}

		inline uint64_t GetSeq() const
		{
			return Protocol::ReadSequence(_headerPtr);
		}

		inline bool IsNew() const
		{
			return Protocol::IsThisNewerThan(GetSeq(), _seq);
		}

		template <typename TCast> requires Tools::PlainOldData<TCast>
		inline SharedArrayEntry<TCast> Cast() const
		{
			return SharedArrayEntry<TCast>(GetEntryPtr(), _access);
		}
	};

	template <typename T> requires Tools::PlainOldData<T>
	class SharedArray final
	{
	public:
		const std::string Name;
		const int32_t Capacity;
		const Tools::Access Access;
        const int32_t TypeSize;
        const bool IsDense;

	private:
		int32_t _entryLength;
		std::vector<SharedArrayEntry<T>> _entries;
		Socket::SharedMemory _mmf;
		Socket::SharedMemoryView _view;
		uint8_t* _basePtr;

	public:
		SharedArray(const std::string& name, int32_t capacity, Tools::Access access, bool isDense = true)
        :
        Name(name),
        Capacity(capacity),
        Access(access),
        TypeSize(static_cast<int32_t>(sizeof(T))),
        IsDense(isDense)
		{
			// 1. Capacity Check
			if (capacity <= 0)
				throw std::invalid_argument("Capacity > 0");
				
			// 2. Calculate Aligned Entry Size
			_entryLength = Protocol::GetAlignedEntryLength(sizeof(T));
			
			// 3. Create or Open Backing Shared Memory and View
			// Compute the total in size_t (avoid int32 overflow), then narrow for the int32 API.
			int32_t totalLength = static_cast<int32_t>(static_cast<size_t>(_entryLength) * static_cast<size_t>(capacity));
			_mmf = Socket::SharedMemory::CreateOrOpen(name, totalLength);
			_view = _mmf.GetView(0, totalLength, access);
			_basePtr = _view.Ptr();

			// 4. Map Individual Entries to Memory Offsets
			_entries.reserve(static_cast<size_t>(capacity));
			for (int32_t i = 0; i < capacity; i++)
			{
				uint8_t* entryPtr = _basePtr + i * _entryLength;
				_entries.emplace_back(entryPtr, access);
			}
		}

		~SharedArray()
		{
			Dispose();
		}

		SharedArray(SharedArray&& other) noexcept = default;
		SharedArray& operator=(SharedArray&& other) noexcept = default;

		SharedArray(const SharedArray&) = delete;
		SharedArray& operator=(const SharedArray&) = delete;

		SharedArrayEntry<T>& operator[](int32_t index)
		{
			return _entries.at(static_cast<size_t>(index));
		}

		SharedArrayEntry<T>& GetEntry(int32_t index)
		{
			return _entries.at(static_cast<size_t>(index));
		}

		void Dispose()
		{
			_view.Dispose();
			_mmf.Dispose();
		}
	};
}