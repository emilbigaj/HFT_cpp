//BEGIN_FILE HFT/Socket/SharedMemory.hpp
#pragma once

#include "Memory.hpp"   // Tools::Memory (resolves to Tools/Memory.hpp; Socket/Memory.hpp is now Protocol.hpp)
#include "Tools.hpp"    // Tools::Access
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>   // MAP_FAILED

namespace Socket
{
	struct SharedMemoryView
	{
	private:
		uint8_t* _ptr;
		int32_t _length;
		Tools::Access _access;
		bool _isDisposed = false;

	public:
		SharedMemoryView() : _ptr(nullptr), _length(0), _access(Tools::Access::Read), _isDisposed(true) {}

		SharedMemoryView(uint8_t* ptr, int32_t length, Tools::Access access) : _ptr(ptr), _length(length), _access(access), _isDisposed(false) {}

		uint8_t* Ptr() const
		{
			if (_isDisposed) throw std::runtime_error("ObjectDisposed");
			return _ptr;
		}

		int32_t Length() const { return _length; }

		Tools::Access Access() const { return _access; }

		bool IsDisposed() const { return _isDisposed; }

		void Dispose()
		{
			if (_isDisposed) return;
			_isDisposed = true;
		}
	};

	// Named shared-memory region for the Socket layer. Thin adapter over Tools::Memory,
	// which owns page selection, the HFT_ namespace, warmup, and the flock-refcount /
	// orphan-reclaim lifecycle. This type just adds the windowed-view API the Socket
	// layer maps its channels through.
	struct SharedMemory
	{
		SharedMemory() = default;

		static SharedMemory CreateOrOpen(const std::string& name, int32_t length)
		{
			SharedMemory sharedMemory;
			sharedMemory._memory = Tools::Memory::CreateOrOpenShared(name, length);
			return sharedMemory;
		}

		// Crash backstop forwarded to the Tools lib; call once on startup, before opening
		// any shared memory.
		static void ReclaimOrphans(std::string_view prefix = Tools::Memory::Namespace)
		{
			Tools::Memory::ReclaimOrphans(prefix);
		}

		uint8_t* Ptr() const { return _memory.Ptr; }
		int32_t Length() const { return _memory.Length; }

		void Clear() { _memory.Clear(); }
		void Dispose() { _memory.Dispose(); }

		SharedMemoryView GetView(int32_t offset, int32_t length, Tools::Access access) const
		{
			if (_memory.Ptr == nullptr || _memory.Ptr == MAP_FAILED)
				throw std::runtime_error("SharedMemory ObjectDisposed");
			// Bounds-check in size_t so offset+length cannot overflow int32.
			if (static_cast<size_t>(offset) + static_cast<size_t>(length) > static_cast<size_t>(_memory.Length))
				throw std::out_of_range("SharedMemoryView out of bounds");
			return SharedMemoryView(_memory.Ptr + offset, length, access);
		}

		SharedMemory(const SharedMemory&) = delete;
		SharedMemory& operator=(const SharedMemory&) = delete;
		SharedMemory(SharedMemory&&) noexcept = default;
		SharedMemory& operator=(SharedMemory&&) noexcept = default;

	private:
		Tools::Memory _memory;
	};
}
//END_FILE HFT/Socket/SharedMemory.hpp
