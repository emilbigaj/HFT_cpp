#pragma once

#include <stdexcept>
#include <algorithm>
#include <span>
#include <type_traits>
#include <utility>

namespace Tools
{
	/// <summary>
	/// High-performance dynamic list with span access and O(1) swap-remove.
	/// </summary>
	/// <remarks>
	/// Requires T to be DefaultConstructible (due to backing array strategy).
	/// Use Emplace to optimize construction of complex types.
	/// </remarks>
	template <typename T>
	class ArrayList final
	{
	private:
		T* _array;
		size_t _capacity;
		size_t _size;
		bool _disposed;

	public:
		// --------- Ctors & Dtors ---------

		ArrayList(size_t initialCapacity = 16)
			: _array(nullptr), _capacity(0), _size(0), _disposed(false)
		{
			if (initialCapacity == 0)
			{
				initialCapacity = 16;
			}

			_array = new T[initialCapacity]; // Note: Default constructs elements
			_capacity = initialCapacity;
		}

		ArrayList(const ArrayList& other)
			: _array(nullptr), _capacity(0), _size(0), _disposed(false)
		{
			if (other._disposed)
			{
				throw std::runtime_error("ObjectDisposedException: ArrayList");
			}

			_capacity = other._capacity;
			_size = other._size;
			_array = new T[_capacity];
			
			if (_size > 0)
			{
				std::copy(other._array, other._array + _size, _array);
			}
		}

		ArrayList(ArrayList&& other) noexcept
			: _array(other._array), _capacity(other._capacity), _size(other._size), _disposed(other._disposed)
		{
			other._array = nullptr;
			other._size = 0;
			other._capacity = 0;
			other._disposed = true;
		}

		~ArrayList()
		{
			Dispose();
		}

		// --------- Properties ---------

		[[nodiscard]] size_t Size() const noexcept
		{
			if (_disposed) return 0; 
			return _size;
		}

		[[nodiscard]] size_t Capacity() const noexcept
		{
			if (_disposed) return 0;
			return _capacity;
		}

		[[nodiscard]] bool IsEmpty() const noexcept
		{
			return _size == 0;
		}

		// ---- Internal helpers (Dangerous Access) ----

		T* DangerousArray()
		{
			ThrowIfDisposed();
			return _array;
		}

		// --------- Basic ops ---------

		[[nodiscard]] bool Contains(const T& item) const
		{
			ThrowIfDisposed();
			for (size_t i = 0; i < _size; i++)
			{
				if (_array[i] == item)
				{
					return true;
				}
			}
			return false;
		}

		void Add(const T& item)
		{
			ThrowIfDisposed();
			if (_size >= _capacity)
			{
				Grow();
			}
			_array[_size++] = item;
		}

		void Add(T&& item)
		{
			ThrowIfDisposed();
			if (_size >= _capacity)
			{
				Grow();
			}
			_array[_size++] = std::move(item);
		}

		/// <summary>
		/// Adds the item only if it is not already present in the list.
		/// Returns true if added, false if it already existed.
		/// </summary>
		bool AddUnique(const T& item)
		{
			if (Contains(item))
			{
				return false;
			}
			Add(item);
			return true;
		}

		/// <summary>
		/// Adds the item only if it is not already present in the list (Move semantics).
		/// Returns true if added, false if it already existed.
		/// </summary>
		bool AddUnique(T&& item)
		{
			// Check existence before moving the object
			if (Contains(item))
			{
				return false;
			}
			Add(std::move(item));
			return true;
		}

		/// <summary>
		/// Constructs an element in-place at the end of the list.
		/// </summary>
		template <typename... Args>
		T& Emplace(Args&&... args)
		{
			ThrowIfDisposed();
			if (_size >= _capacity)
			{
				Grow();
			}
			
			// Construct temp and move-assign. 
			_array[_size] = T(std::forward<Args>(args)...);
			return _array[_size++];
		}

		void AddRange(std::span<const T> items)
		{
			ThrowIfDisposed();
			size_t needed = _size + items.size();
			
			if (needed < _size) 
			{
				throw std::bad_alloc();
			}

			if (_capacity < needed)
			{
				EnsureCapacity(needed);
			}

			std::copy(items.begin(), items.end(), _array + _size);
			_size = needed;
		}

		std::span<T> AddUninitialized(size_t count)
		{
			ThrowIfDisposed();
			size_t start = _size;
			size_t needed = start + count;

			if (needed < start)
			{
				throw std::bad_alloc();
			}

			if (_capacity < needed)
			{
				EnsureCapacity(needed);
			}

			_size = needed;
			return std::span<T>(_array + start, count);
		}

		void Clear()
		{
			ThrowIfDisposed();
			if constexpr (!std::is_trivially_destructible_v<T>)
			{
				std::fill(_array, _array + _size, T());
			}
			_size = 0;
		}

		void SwapRemoveAt(size_t index)
		{
			ThrowIfDisposed();
			if (index >= _size)
			{
				ThrowOutOfRange();
			}

			size_t last = _size - 1;
			if (index < last)
			{
				_array[index] = std::move(_array[last]);
			}

			if constexpr (!std::is_trivially_destructible_v<T>)
			{
				_array[last] = T();
			}

			_size = last;
		}

        [[nodiscard]] T* TryGet(size_t index)
        {
            ThrowIfDisposed();

            // Only one check needed. 
            // If index >= _size, it is invalid regardless of capacity.
            if (index >= _size)
            {
                return nullptr;
            }

            // Return address of the item (Zero overhead)
            return &_array[index];
        }

        // Overload for const correctness (Read-only access)
        [[nodiscard]] const T* TryGet(size_t index) const
        {
            ThrowIfDisposed();

            if (index >= _size)
            {
                return nullptr;
            }

            return &_array[index];
        }

		T RemoveAt(size_t index)
		{
			ThrowIfDisposed();
			if (index >= _size)
			{
				ThrowOutOfRange();
			}

			T value = std::move(_array[index]);
			size_t last = _size - 1;

			if (index < last)
			{
				std::move(_array + index + 1, _array + _size, _array + index);
			}

			if constexpr (!std::is_trivially_destructible_v<T>)
			{
				_array[last] = T();
			}

			_size = last;
			return value;
		}

		[[nodiscard]] bool Remove(const T& item)
		{
			ThrowIfDisposed();
			for (size_t i = 0; i < _size; i++)
			{
				if (_array[i] == item)
				{
					RemoveAt(i);
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] bool SwapRemove(const T& item)
		{
			ThrowIfDisposed();
			for (size_t i = 0; i < _size; i++)
			{
				if (_array[i] == item)
				{
					SwapRemoveAt(i);
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] bool TryPop(T& value)
		{
			ThrowIfDisposed();
			if (_size == 0)
			{
				value = T();
				return false;
			}

			size_t last = _size - 1;
			value = std::move(_array[last]);

			if constexpr (!std::is_trivially_destructible_v<T>)
			{
				_array[last] = T();
			}

			_size = last;
			return true;
		}

		T& operator[](size_t index)
		{
			ThrowIfDisposed();
			if (index >= _size)
			{
				ThrowOutOfRange();
			}
			return _array[index];
		}

		const T& operator[](size_t index) const
		{
			ThrowIfDisposed();
			if (index >= _size)
			{
				ThrowOutOfRange();
			}
			return _array[index];
		}

		std::span<T> AsSpan()
		{
			ThrowIfDisposed();
			return std::span<T>(_array, _size);
		}

		std::span<const T> AsReadOnlySpan() const
		{
			ThrowIfDisposed();
			return std::span<const T>(_array, _size);
		}

		// ---- Insert APIs ----

		void InsertAt(size_t index, const T& item)
		{
			ThrowIfDisposed();
			if (index > _size)
			{
				ThrowOutOfRange();
			}

			if (_size == _capacity)
			{
				EnsureCapacity(_size + 1);
			}

			if (index < _size)
			{
				std::move_backward(_array + index, _array + _size, _array + _size + 1);
			}

			_array[index] = item;
			_size++;
		}

		template <typename... Args>
		void EmplaceAt(size_t index, Args&&... args)
		{
			ThrowIfDisposed();
			if (index > _size)
			{
				ThrowOutOfRange();
			}

			if (_size == _capacity)
			{
				EnsureCapacity(_size + 1);
			}

			if (index < _size)
			{
				std::move_backward(_array + index, _array + _size, _array + _size + 1);
			}

			_array[index] = T(std::forward<Args>(args)...);
			_size++;
		}

		// ---------- Sorting ----------

		template <typename Comparator>
		void Sort(Comparator comp)
		{
			ThrowIfDisposed();
			if (_size <= 1)
			{
				return;
			}
			std::sort(_array, _array + _size, comp);
		}

		void Sort()
		{
			ThrowIfDisposed();
			if (_size <= 1)
			{
				return;
			}
			std::sort(_array, _array + _size);
		}

		// ---------- Capacity management ----------

		void Reserve(size_t min)
		{
			EnsureCapacity(min);
		}

		void EnsureCapacity(size_t min)
		{
			ThrowIfDisposed();
			if (_capacity < min)
			{
				ResizeBuffer(ComputeNewSize(_capacity, min));
			}
		}

		// ---------- Enumeration (Standard C++ Iterators) ----------

		using iterator = T*;
		using const_iterator = const T*;

		iterator begin() { ThrowIfDisposed(); return _array; }
		iterator end() { ThrowIfDisposed(); return _array + _size; }
		const_iterator begin() const { ThrowIfDisposed(); return _array; }
		const_iterator end() const { ThrowIfDisposed(); return _array + _size; }
		const_iterator cbegin() const { ThrowIfDisposed(); return _array; }
		const_iterator cend() const { ThrowIfDisposed(); return _array + _size; }

		// ---------- Disposal ----------

		void Dispose()
		{
			if (_disposed)
			{
				return;
			}
			_disposed = true;

			if (_array != nullptr)
			{
				delete[] _array;
				_array = nullptr;
			}
			_size = 0;
			_capacity = 0;
		}

	private:
		void Grow()
		{
			size_t newSize = ComputeNewSize(_capacity, _size + 1);
			ResizeBuffer(newSize);
		}

		static size_t ComputeNewSize(size_t cur, size_t needed)
		{
			if (needed < cur) 
			{
				throw std::bad_alloc();
			}

			size_t next = (cur <= 1024) ? (cur == 0 ? 4 : cur << 1) : cur + (cur >> 1);
			
			if (next < needed)
			{
				next = needed;
			}

			if (next < cur)
			{
				next = needed;
			}

			return next;
		}

		void ResizeBuffer(size_t newSize)
		{
			T* newArr = new T[newSize];

			if (_size > 0)
			{
				std::move(_array, _array + _size, newArr);
			}

			if (_array != nullptr)
			{
				delete[] _array;
			}

			_array = newArr;
			_capacity = newSize;
		}

		static void ThrowOutOfRange()
		{
			throw std::out_of_range("Index out of range");
		}

		void ThrowIfDisposed() const
		{
			if (_disposed)
			{
				throw std::runtime_error("ObjectDisposedException: ArrayList");
			}
		}
	};
}