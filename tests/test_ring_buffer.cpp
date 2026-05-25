#include "../src/core/ring_buffer.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

struct ThreadData {
    RingBuffer<int>* buffer;
    int totalItems;
    std::vector<int>* output;
};

void producer_thread(ThreadData* data) {
    for (int i = 0; i < data->totalItems; ++i) {
        while (!data->buffer->write(i)) {
            std::this_thread::yield();
        }
    }
}

void consumer_thread(ThreadData* data) {
    int item = 0;
    for (int count = 0; count < data->totalItems; ) {
        if (data->buffer->read(item)) {
            data->output->push_back(item);
            ++count;
        } else {
            std::this_thread::yield();
        }
    }
}

int main() {
    RingBuffer<int> buffer(128);
    int initial = 0;
    assert(!buffer.read(initial)); // empty at startup

    int value = 42;
    assert(buffer.write(value));
    int received = 0;
    assert(buffer.read(received));
    assert(received == value);
    assert(!buffer.read(received));

    constexpr int kTotalItems = 100000;
    std::vector<int> consumed;
    consumed.reserve(kTotalItems);

    ThreadData producerData{&buffer, kTotalItems, nullptr};
    ThreadData consumerData{&buffer, kTotalItems, &consumed};

    std::thread producer(producer_thread, &producerData);
    std::thread consumer(consumer_thread, &consumerData);

    producer.join();
    consumer.join();

    assert(consumed.size() == kTotalItems);
    for (int i = 0; i < kTotalItems; ++i) {
        assert(consumed[i] == i);
    }

    std::cout << "Ring buffer test passed: " << kTotalItems << " ordered items." << std::endl;
    return 0;
}
