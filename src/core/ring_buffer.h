#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>

// Single-producer, single-consumer lock-free ring buffer.
// The buffer capacity is configurable at construction and the implementation
// never blocks: write() returns false when full, read() returns false when empty.

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity);
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // Return False if full, drops item. Nonblocking
    bool write(const T& item); 

    // Return False if empty, item unchanged. Nonblocking
    bool read(T& item);

    // Effective usable capacity, which is one less than the allocated storage.
    size_t capacity() const noexcept { return capacity_ - 1; }

private:
    size_t next_index(size_t index) const noexcept {
        return (index + 1) % capacity_;
    }

    const size_t capacity_;
    std::unique_ptr<T[]> buffer_; // exclusively owned, no re-sizing 
    std::atomic<size_t> write_ptr_{0}; 
    std::atomic<size_t> read_ptr_{0};
};

#include "ring_buffer.cpp"
