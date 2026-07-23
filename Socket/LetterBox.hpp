//BEGIN_FILE HFT/Socket/LetterBox.hpp
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include "Protocol.hpp"
#include "SeqLock.hpp"
#include "Tools.hpp"
#include "SharedMemory.hpp"
#include "SharedArray.hpp"

namespace Socket
{
	template <typename T> requires Tools::PlainOldData<T>
	class LetterBox final
	{
	public:
		const std::string Name;
		const Tools::Access Access;

	private:
		bool _disposed = false;
		SharedMemory _sharedMemory;
		SharedMemoryView _view;
		Protocol::Header64* _headerPtr;
		T* _valuePtr;
		SharedArrayEntry<T> _entry;

	public:
		explicit LetterBox(const std::string& name, Tools::Access access) : Name(name), Access(access), _entry(nullptr, access)
		{
			// 1. Calculate Required Dimensions
			std::string shmName = Name + "LetterBox";
			int32_t totalLength = Protocol::GetAlignedEntryLength(sizeof(T));

			// 2. Open Shared Memory and View
			_sharedMemory = Socket::SharedMemory::CreateOrOpen(shmName, totalLength);
			_view = _sharedMemory.GetView(0, totalLength, Access);
			
			// 3. Map Internal Pointers
			uint8_t* basePtr = _view.Ptr();
			_headerPtr = reinterpret_cast<Protocol::Header64*>(basePtr);
			_valuePtr = reinterpret_cast<T*>(basePtr + sizeof(Protocol::Header64));
			_entry = SharedArrayEntry<T>(reinterpret_cast<uint8_t*>(_headerPtr), _view.Access());
		}

		~LetterBox()
		{
			Dispose();
		}

		void Dispose()
		{
			if (_disposed)
				return;
				
			// 1. Dispose Resource Handles
			_view.Dispose();
			_sharedMemory.Dispose();
			
			// 2. Clear State
			_headerPtr = nullptr;
			_valuePtr = nullptr;
			_disposed = true;
		}

		LetterBox(LetterBox&& other) noexcept : Name(std::move(other.Name)), Access(other.Access), _entry(nullptr, other.Access)
		{
			MoveFrom(std::move(other));
		}

		LetterBox& operator=(LetterBox&& other) noexcept
		{
			if (this != &other)
			{
				Dispose();
				// Note: const fields Name and Access cannot be assigned. Re-creation is required for strict semantics.
			}
			return *this;
		}

		LetterBox(const LetterBox&) = delete;
		LetterBox& operator=(const LetterBox&) = delete;

		void AcquireLock()
		{
			Tools::MultiSeqLockWriter::AcquireLock(&_headerPtr->Sequence);
		}

		void ReleaseLock()
		{
			Tools::MultiSeqLockWriter::ReleaseLock(&_headerPtr->Sequence);
		}

		bool IsEmpty() const	
		{
			return _headerPtr->Length == 0;
		}

		T& GetRef()
		{
			if (_disposed) throw std::runtime_error("ObjectDisposed");
			if (_view.Access() == Tools::Access::Read) throw std::invalid_argument("Readonly");
			return *_valuePtr;
		}

		const T& GetReadonlyRef()
		{
			if (_disposed) throw std::runtime_error("ObjectDisposed");
			return *_valuePtr;
		}

		SharedArrayEntry<T>& GetEntry()
		{
			if (_disposed) throw std::runtime_error("ObjectDisposed");
			return _entry;
		}

		bool TryPeek(T& value)
		{
			value = T();

			// 1. Validate Object Lifecycle
			if (_disposed)
				throw std::runtime_error("ObjectDisposed");
				
			SharedArrayEntry<T>& entry = GetEntry();
			
			while(true)
			{
				// 2. Read Sequence
				uint64_t seq0 = entry.GetSeq();
				
				// 3. Spin if Writer is Currently Active
				if (Protocol::IsWriteInProgress(seq0))
				{
					_mm_pause();
					continue;
				}
				
				// 4. Safely Read Payload
				bool isEmpty = IsEmpty();
				value = *_valuePtr;
				std::atomic_thread_fence(std::memory_order_acquire);
				
				// 5. Post-Read Sequence Validation
				uint64_t seq1 = entry.GetSeq();
				if (seq0 == seq1)
					return !isEmpty;
			}
		}

		bool TryStore(const T& value)
		{
			// 1. Verify Access Rights and Lifecycle State
			if (_view.Access() == Tools::Access::Read)
				throw std::runtime_error("LetterBox " + Name + " is in readonly mode");
			if (_disposed)
				throw std::runtime_error("ObjectDisposed");
				
			// 2. Acquire Sequence Lock (Multi-writer safe)
			Tools::RAIILock<Tools::MultiSeqLockWriter> guard(&_headerPtr->Sequence);
			
			// 3. Abort if Already Full
			if (!IsEmpty())
				return false;
				
			// 4. Write Payload and Update Length
			*_valuePtr = value;
			_headerPtr->Magic.store(Protocol::s_magic, std::memory_order_relaxed);
			_headerPtr->Length = sizeof(T);
			return true;
		}

		bool TryEmpty(T& outValue)
		{
			outValue = T();
			
			// 1. Validate Lifecycle State
			if (_disposed)
				throw std::runtime_error("ObjectDisposed");

			// 2. Lock-free early exit check (saves pounding the SeqLock)
			if (IsEmpty())
				return false;
				
			// 3. Acquire global SeqLock for Letterbox mutation
			Tools::RAIILock<Tools::MultiSeqLockWriter> guard(&_headerPtr->Sequence);
			
			// 4. Bail if empty (double-check after lock)
			if (IsEmpty())
				return false;
				
			// 5. Move payload out and mark as empty
			outValue = *_valuePtr;
			
			_headerPtr->Magic.store(0, std::memory_order_relaxed);
			_headerPtr->Length = 0;
			return true;
		}

	private:
		void MoveFrom(LetterBox&& other)
		{
			_headerPtr = other._headerPtr;
			_valuePtr = other._valuePtr;
			_entry = other._entry;
			_disposed = other._disposed;
			_sharedMemory = std::move(other._sharedMemory);
			_view = std::move(other._view);
			other._headerPtr = nullptr;
			other._disposed = true;
		}
	};
}
//END_FILE HFT/Socket/LetterBox.hpp