#pragma once

#include "ring_buffer.h"

#include <utility>

template <typename T>
RingBuffer<T>::RingBuffer(size_t capacity)
    : capacity_(capacity + 1),
      buffer_(std::make_unique<T[]>(capacity_)) {
}

// writes to specific spot in buffer and advances write pointer
template <typename T>
bool RingBuffer<T>::write(const T& item) {
    // acquire after a reader's release
    // ensure we see latest read_ptr before checking if buffer is full
    size_t readPos = read_ptr_.load(std::memory_order_acquire);
    size_t writePos = write_ptr_.load(std::memory_order_relaxed);
    size_t nextWrite = next_index(writePos);

    if (nextWrite == readPos) {
        return false; // buffer is full
    }

    buffer_[writePos] = item;
    write_ptr_.store(nextWrite, std::memory_order_release);
    return true;
}

// reads from specific spot in buffer and advances read pointer
// ** changes the item reference if read successful
template <typename T>
bool RingBuffer<T>::read(T& item) {
    // acquire after a writer's release
    // ensure we see latest write_ptr before checking if buffer is empty
    size_t writePos = write_ptr_.load(std::memory_order_acquire);
    size_t readPos = read_ptr_.load(std::memory_order_relaxed);

    if (readPos == writePos) {
        return false; // buffer is empty
    }

    item = buffer_[readPos];
    read_ptr_.store(next_index(readPos), std::memory_order_release);
    return true;
}
