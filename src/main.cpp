#include "core/ring_buffer.h"
#include <array>
#include <iostream>

int main() {
    using AudioFrame = std::array<float, 480>;

    constexpr size_t frameBufferSize = 256;
    RingBuffer<AudioFrame> rb1(frameBufferSize);
    RingBuffer<AudioFrame> rb2(frameBufferSize);

    std::cout << "RB1 capacity = " << rb1.capacity() << " frames\n";
    std::cout << "RB2 capacity = " << rb2.capacity() << " frames\n";
    std::cout << "Ring buffers initialized successfully." << std::endl;

    return 0;
}
