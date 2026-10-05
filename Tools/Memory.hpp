//BEGIN_FILE HFT/Tools/Memory.hpp
#pragma once

#include "Tools.hpp"
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef MAP_TYPE
#undef MAP_TYPE
#endif

namespace Tools
{
	// General-purpose flock-based named mutex. The lock file is /dev/shm/HFT_Lock_<name>;
	// flock(LOCK_EX) gives cross-process mutual exclusion for the life of the object. The
	// constructor is defined out-of-line below so it can reference Memory::Namespace /
	// Memory::LockInfix (Memory is declared after this class but uses MutexFLock).
	class MutexFLock
	{
		int _fd;
		std::string _path;

	public:
		explicit MutexFLock(const std::string& name);

		~MutexFLock()
		{
			if (_fd != -1)
			{
				flock(_fd, LOCK_UN);
				close(_fd);
			}
		}

		MutexFLock(const MutexFLock&) = delete;
		MutexFLock& operator=(const MutexFLock&) = delete;
	};

	// Page-backed memory region. Two flavours:
	//   * Anonymous    - MAP_PRIVATE | MAP_ANONYMOUS [| MAP_HUGETLB], no file, no name.
	//   * Named shared - backed by /dev/hugepages/HFT_<name> or /dev/shm/HFT_<name>,
	//                    MAP_SHARED [| MAP_HUGETLB], with a flock-refcount + orphan
	//                    reclaim lifecycle so huge pages return to the pool on exit.
	// Huge pages are selected automatically when the requested length >= HugePageLength.
	class Memory
	{
	public:
		// ---- constants (referenced as Tools::Memory::X; mirrors a C# static class) ----

		// App-owned tag on every NAMED file we create — region files (HFT_<name>) and lock
		// files (HFT_Lock_<name>) — so a startup sweep can tell our segments apart from any
		// other process's in /dev/shm or /dev/hugepages. Anonymous mappings have no file.
		static constexpr std::string_view Namespace = "HFT_";
		static constexpr std::string_view LockInfix = "Lock_";

		static constexpr int32_t HugePageLength = 2 * 1024 * 1024;   // default x86 huge page
		static constexpr int32_t SmallPageLength = 4 * 1024;
		static constexpr int32_t CacheLine = 64;

		// Rounds `length` up to a multiple of `alignment` (which must be a power of two). The
		// single rounding primitive reused by ByteQueue, Protocol, and the factories below.
		// Branchless: the add is done in size_t so it never overflows. Callers that accept
		// arbitrary lengths (AlignLength) guard the upper bound once, on their cold path, so
		// the int32 result can't truncate; hot-path callers pass values far below INT32_MAX.
		[[nodiscard]] ALWAYS_INLINE static int32_t GetAlignedLength(int32_t length, int32_t alignment)
		{
			size_t rounded = (static_cast<size_t>(length) + static_cast<size_t>(alignment - 1)) & ~static_cast<size_t>(alignment - 1);
			return static_cast<int32_t>(rounded);
		}

		int32_t FileDescriptor;   // -1 for anonymous
		uint8_t* Ptr;
		int32_t Length;
		bool Huge;

		// Backing-file identity, kept so Dispose() can reclaim the file (named only).
		std::string Path;   // full path, e.g. /dev/hugepages/HFT_<name>; empty if anonymous
		std::string Name;   // sanitised logical name; keys the MutexFLock (HFT_Lock_<Name>)

		Memory() : FileDescriptor(-1), Ptr(nullptr), Length(0), Huge(false) {}

		// ---- factories ----

		static Memory CreateAnonymous(int32_t length)
		{
			MLock();
			bool useHugePages = length >= HugePageLength;
			length = AlignLength(length, useHugePages);

			int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE;
			if (useHugePages) flags |= MAP_HUGETLB;

			void* ptr = mmap(nullptr, static_cast<size_t>(length), PROT_READ | PROT_WRITE, flags, -1, 0);
			if (ptr == MAP_FAILED)
				throw std::runtime_error("Memory::CreateAnonymous: mmap failed: " + std::string(std::strerror(errno)) + ". Ensure hugepages are available for large regions.");

			Memory memory;
			memory.Ptr = static_cast<uint8_t*>(ptr);
			memory.Length = length;
			memory.Huge = useHugePages;
			memory.WarmUp();
			return memory;
		}

		static Memory CreateOrOpenShared(const std::string& name, int32_t length)
		{
			MLock();
			bool useHugePages = length >= HugePageLength;
			length = AlignLength(length, useHugePages);

			std::string lockName = Tools::Sanitize(name);
			// Region files are HFT_<lockName>; lock files are HFT_Lock_<lockName>. A region
			// whose sanitised name began with the lock infix would collide with a lock-file
			// path (and be skipped forever by the reclaim sweep), so reject it.
			if (lockName.rfind(std::string(LockInfix), 0) == 0)
				throw std::invalid_argument("Memory::CreateOrOpenShared: region name must not start with '" + std::string(LockInfix) + "' (reserved): " + name);

			std::string fileName = std::string(Namespace) + lockName;
			std::string fullPath = std::string(useHugePages ? "/dev/hugepages/" : "/dev/shm/") + fileName;

			// Drop a stale orphan left by a crashed previous owner so we map a clean inode.
			// Done under its own lock acquisition BEFORE we take `regionLock` below: re-locking
			// the same name within one process via flock would deadlock.
			TryUnlinkIfOrphan(fullPath, lockName);

			// Hold the region lock across open -> LOCK_SH -> mmap so a concurrent reclaim
			// cannot unlink the file between our create and our shared lock (which is what
			// would split two openers onto different inodes).
			MutexFLock regionLock(lockName);

			int fileDescriptor = open(fullPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
			if (fileDescriptor == -1)
				throw std::runtime_error("Memory::CreateOrOpenShared: open failed: " + fullPath + ": " + std::strerror(errno));

			struct stat fileStatus{};
			if (fstat(fileDescriptor, &fileStatus) == -1)
			{
				int error = errno; close(fileDescriptor);
				throw std::runtime_error("Memory::CreateOrOpenShared: fstat failed: " + fullPath + ": " + std::strerror(error));
			}
			if (fileStatus.st_size < static_cast<off_t>(length))
			{
				if (ftruncate(fileDescriptor, static_cast<off_t>(length)) == -1)
				{
					int error = errno; close(fileDescriptor);
					throw std::runtime_error("Memory::CreateOrOpenShared: ftruncate failed: " + fullPath + ": " + std::strerror(error));
				}
			}

			// LOCK_SH is the refcount the orphan-reclaim protocol relies on; a silent failure
			// would let a concurrent reclaim unlink this live region. Retry on EINTR, fail hard.
			int sharedLockResult;
			do { sharedLockResult = flock(fileDescriptor, LOCK_SH); } while (sharedLockResult == -1 && errno == EINTR);
			if (sharedLockResult == -1)
			{
				int error = errno; close(fileDescriptor);
				throw std::runtime_error("Memory::CreateOrOpenShared: LOCK_SH failed: " + fullPath + ": " + std::strerror(error));
			}

			int flags = MAP_SHARED | MAP_POPULATE;
			if (useHugePages) flags |= MAP_HUGETLB;

			void* ptr = mmap(nullptr, static_cast<size_t>(length), PROT_READ | PROT_WRITE, flags, fileDescriptor, 0);
			if (ptr == MAP_FAILED)
				throw std::runtime_error("Memory::CreateOrOpenShared: mmap failed: " + fullPath);

			Memory memory;
			memory.FileDescriptor = fileDescriptor;
			memory.Ptr = static_cast<uint8_t*>(ptr);
			memory.Length = length;
			memory.Huge = useHugePages;
			memory.Path = std::move(fullPath);
			memory.Name = std::move(lockName);
			memory.WarmUp();
			return memory;
		}

		// ---- lifecycle ----

		// Faults every page in (and pins it to the local NUMA node) to keep the runtime
		// jitter-free. MADV_POPULATE_WRITE does it in one syscall; the fallback strides
		// one touch per page.
		void WarmUp()
		{
			if (Ptr == nullptr || Ptr == MAP_FAILED) return;

			if (madvise(Ptr, static_cast<size_t>(Length), MADV_POPULATE_WRITE) == 0)
				return;

			size_t stride = static_cast<size_t>(Huge ? HugePageLength : SmallPageLength);
			for (size_t offset = 0; offset < static_cast<size_t>(Length); offset += stride)
			{
				std::atomic_ref<int> atomicValue(*reinterpret_cast<int*>(Ptr + offset));
				int expected = atomicValue.load(std::memory_order_relaxed);
				while (!atomicValue.compare_exchange_weak(expected, expected, std::memory_order_release, std::memory_order_relaxed))
					continue;
			}
		}

		void Clear()
		{
			if (Ptr != nullptr && Ptr != MAP_FAILED)
				std::memset(Ptr, 0, static_cast<size_t>(Length));
		}

		// Releases this process's mapping and, for named regions, attempts to reclaim the
		// backing file. The reclaim is gated by an exclusive-lock probe, so only the LAST
		// holder unlinks; unlinking is what returns huge pages to the pool (munmap alone
		// never does, because the inode keeps the pages pinned).
		void Dispose()
		{
			if (Ptr == nullptr && FileDescriptor == -1 && Path.empty())
				return;

			// Grab identity locally and blank the members so a second Dispose() is a no-op.
			std::string path;
			std::string name;
			path.swap(Path);
			name.swap(Name);

			// Drop our own mapping and our LOCK_SH. Closing the fd releases the shared lock,
			// and this MUST happen before the LOCK_EX probe: flock treats separate open
			// descriptions independently even within one process, so our own LOCK_SH would
			// otherwise block our own exclusive probe.
			if (Ptr && Ptr != MAP_FAILED) munmap(Ptr, static_cast<size_t>(Length));
			if (FileDescriptor != -1) close(FileDescriptor);
			Ptr = nullptr; FileDescriptor = -1; Length = 0;

			if (path.empty()) return;   // anonymous -> nothing to reclaim

			// Best-effort: TryUnlinkIfOrphan takes the region lock (can throw via MutexFLock)
			// and only the last user wins the probe. Dispose() runs from the destructor, so
			// it must never throw.
			try
			{
				TryUnlinkIfOrphan(path, name);
			}
			catch (...)
			{
				// Swallow: reclaim is best-effort, the startup sweep is the backstop.
			}
		}

		~Memory() { Dispose(); }

		Memory(const Memory&) = delete;
		Memory& operator=(const Memory&) = delete;

		Memory(Memory&& other) noexcept
			: FileDescriptor(other.FileDescriptor), Ptr(other.Ptr), Length(other.Length), Huge(other.Huge),
			  Path(std::move(other.Path)), Name(std::move(other.Name))
		{
			other.FileDescriptor = -1;
			other.Ptr = nullptr;
			other.Length = 0;
			other.Huge = false;
			other.Path.clear();
			other.Name.clear();
		}

		Memory& operator=(Memory&& other) noexcept
		{
			if (this != &other)
			{
				Dispose();
				FileDescriptor = other.FileDescriptor;
				Ptr = other.Ptr;
				Length = other.Length;
				Huge = other.Huge;
				Path = std::move(other.Path);
				Name = std::move(other.Name);
				other.FileDescriptor = -1;
				other.Ptr = nullptr;
				other.Length = 0;
				other.Huge = false;
				other.Path.clear();
				other.Name.clear();
			}
			return *this;
		}

		// ---- naming + reclaim ----

		// Pins all current + future pages once per process. std::call_once makes the first
		// call win race-free and re-runs only if it threw (mlockall is idempotent otherwise).
		static void MLock()
		{
			static std::once_flag s_mlockOnce;
			std::call_once(s_mlockOnce, []()
			{
				if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
					throw std::runtime_error("mlockall failed: " + std::string(std::strerror(errno)));
			});
		}

		// Unlink `path` iff no process still maps the region — the inode is what pins the
		// pages, so unlink is what returns them to the pool. A file is an orphan iff we can
		// take LOCK_EX on it: every live mapper holds LOCK_SH for the life of its mapping,
		// so an exclusive lock proves there are no mappers. The MutexFLock serialises us
		// against a concurrent CreateOrOpenShared of the same name. Returns true iff this
		// call unlinked the file.
		static bool TryUnlinkIfOrphan(const std::string& path, const std::string& lockName)
		{
			MutexFLock regionLock(lockName);

			int fileDescriptor = open(path.c_str(), O_RDWR | O_CLOEXEC);
			if (fileDescriptor == -1)
				return false;   // already gone, or not openable -> nothing to reclaim

			bool unlinked = false;
			if (flock(fileDescriptor, LOCK_EX | LOCK_NB) == 0)   // no LOCK_SH holders -> no live mappers
			{
				unlink(path.c_str());                            // inode dropped -> pages return to pool
				flock(fileDescriptor, LOCK_UN);
				unlinked = true;
			}
			close(fileDescriptor);
			return unlinked;
		}

		// Crash backstop. A process that dies without running Dispose() leaves its backing
		// file in place; the inode pins its pages out of the pool indefinitely. Call this
		// ONCE on startup, BEFORE opening any shared memory, to unlink every orphan no live
		// process still holds, across both the huge-page and small-page mounts. `prefix`
		// restricts the sweep to this app's region files (defaults to the HFT_ namespace).
		static void ReclaimOrphans(std::string_view prefix = Namespace)
		{
			ReclaimOrphansIn("/dev/hugepages", prefix);
			ReclaimOrphansIn("/dev/shm", prefix);
		}

	private:
		static int32_t AlignLength(int32_t length, bool useHugePages)
		{
			// Cold path (region allocation): validate the caller-supplied length here — once —
			// so the rounded result always fits int32 and GetAlignedLength stays branchless.
			// Huge regions round to a whole 2 MB page, small regions to a cache line.
			int32_t alignment = useHugePages ? HugePageLength : CacheLine;
			if (length <= 0 || length > INT32_MAX - alignment)
				throw std::invalid_argument("Memory::AlignLength: length out of range (must be > 0 and leave room to round up within int32)");
			return GetAlignedLength(length, alignment);
		}

		// Sweep one directory: probe-and-unlink every orphan whose name matches `prefix`.
		// Skips the lock files (HFT_Lock_*) — they must never be unlinked out from under a
		// live region's lock.
		static void ReclaimOrphansIn(const std::string& directory, std::string_view prefix)
		{
			DIR* directoryStream = opendir(directory.c_str());
			if (directoryStream == nullptr) return;

			std::string lockPrefix = std::string(Namespace) + std::string(LockInfix); // "HFT_Lock_"
			std::string namespacePrefix = std::string(Namespace);                      // "HFT_"
			std::string prefixString(prefix);

			struct dirent* entry;
			while ((entry = readdir(directoryStream)) != nullptr)
			{
				std::string fileName = entry->d_name;
				if (fileName == "." || fileName == "..") continue;
				if (fileName.rfind(lockPrefix, 0) == 0) continue;                 // never our own locks
				if (!prefixString.empty() && fileName.rfind(prefixString, 0) != 0) continue;
				if (fileName.rfind(namespacePrefix, 0) != 0) continue;            // only HFT_ region files

				// Region file is Namespace + lockName, so strip the namespace to recover the
				// lock key CreateOrOpenShared / Dispose use for this region.
				std::string lockName = fileName.substr(namespacePrefix.size());
				try
				{
					TryUnlinkIfOrphan(directory + "/" + fileName, lockName);
				}
				catch (...)
				{
					continue;   // could not lock/inspect; skip and keep sweeping
				}
			}

			closedir(directoryStream);
		}
	};

	// Defined out-of-line so it can reference Memory::Namespace / Memory::LockInfix (Memory
	// is declared after MutexFLock but depends on it). Lock file: /dev/shm/HFT_Lock_<name>.
	inline MutexFLock::MutexFLock(const std::string& name) : _fd(-1)
	{
		_path = std::string("/dev/shm/") + std::string(Memory::Namespace) + std::string(Memory::LockInfix) + name;
		_fd = open(_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
		if (_fd == -1)
			throw std::runtime_error("MutexFLock: failed to open lock file " + _path + ": " + std::strerror(errno));

		int lockResult;
		do { lockResult = flock(_fd, LOCK_EX); } while (lockResult == -1 && errno == EINTR);
		if (lockResult == -1)
		{
			int error = errno;
			close(_fd);
			throw std::runtime_error("MutexFLock: failed to acquire lock on " + _path + ": " + std::strerror(error));
		}
	}
}
//END_FILE HFT/Tools/Memory.hpp
