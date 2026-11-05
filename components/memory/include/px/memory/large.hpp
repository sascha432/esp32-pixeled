#pragma once

// The heap for large, long-lived data: the dashboard model and the trees of an import
// (docs/architecture.md section 11: the dashboard lives in PSRAM). Plain C++, host tested.
//
// On the device main points it at PSRAM before any task starts; on the PC it is malloc. The
// containers below allocate from it, so the code that uses them does not know where it lives.
// Small values that are read often stay out of it, in internal RAM.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace px::memory {

using AllocateFunction = void *(*)(size_t size);
using ReleaseFunction = void (*)(void *pointer);

// Called once at start, before anything allocates from the large heap.
void set_large_heap(AllocateFunction allocate, ReleaseFunction release);

// Null when the heap is full: the caller says so ("no memory for the frame") and goes on.
void *allocate_large(size_t size);
// Null is ignored.
void release_large(void *pointer);

template <class T>
struct LargeAllocator {
    using value_type = T;

    LargeAllocator() = default;
    template <class U>
    LargeAllocator(const LargeAllocator<U> &) noexcept
    {
    }

    // A container cannot report a full heap (no exceptions on the device): it aborts. What grows
    // with the LEDs and may not fit uses allocate_large or Array (below) instead.
    T *allocate(size_t count)
    {
        void *pointer = allocate_large(count * sizeof(T));
        if (pointer == nullptr) {
            std::abort();
        }
        return static_cast<T *>(pointer);
    }
    void deallocate(T *pointer, size_t) noexcept
    {
        release_large(pointer);
    }

    template <class U>
    bool operator==(const LargeAllocator<U> &) const noexcept
    {
        return true;
    }
};

template <class T>
using Vector = std::vector<T, LargeAllocator<T>>;
using String = std::basic_string<char, std::char_traits<char>, LargeAllocator<char>>;

// A fixed number of plain values on the large heap that, unlike Vector, survives a full heap:
// assign() answers false then. For the tables that grow with the LEDs (the layout). Move only.
template <class T>
class Array {
    static_assert(std::is_trivially_copyable_v<T>, "plain values only: no constructors are run");

public:
    Array() = default;
    Array(const Array &) = delete;
    Array &operator=(const Array &) = delete;
    Array(Array &&other) noexcept : _data(std::exchange(other._data, nullptr)), _size(std::exchange(other._size, 0))
    {
    }
    Array &operator=(Array &&other) noexcept
    {
        if (this != &other) {
            release_large(_data);
            _data = std::exchange(other._data, nullptr);
            _size = std::exchange(other._size, 0);
        }
        return *this;
    }
    ~Array()
    {
        release_large(_data);
    }

    // `count` copies of `value`; false and empty when the heap has no room
    bool assign(size_t count, const T &value)
    {
        if (!reserve(count)) {
            return false;
        }
        std::fill_n(_data, count, value);
        return true;
    }
    bool assign(std::initializer_list<T> values)
    {
        if (!reserve(values.size())) {
            return false;
        }
        std::copy(values.begin(), values.end(), _data);
        return true;
    }

    size_t size() const
    {
        return _size;
    }
    T &operator[](size_t index)
    {
        return _data[index];
    }
    const T &operator[](size_t index) const
    {
        return _data[index];
    }
    const T *begin() const
    {
        return _data;
    }
    const T *end() const
    {
        return _data + _size;
    }

private:
    bool reserve(size_t count)
    {
        release_large(_data);
        _data = count <= SIZE_MAX / sizeof(T) ? static_cast<T *>(allocate_large(count * sizeof(T))) : nullptr;
        _size = _data != nullptr ? count : 0;
        return _data != nullptr;
    }

    T *_data = nullptr;
    size_t _size = 0;
};

} // namespace px::memory
