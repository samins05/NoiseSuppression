#include "processing_stage.h"

#include <thread>

ProcessingStage::ProcessingStage(RingBuffer<AudioFrame>& input,
                                 RingBuffer<AudioFrame>& output,
                                 Suppressor& suppressor)
    : input_(input), output_(output), suppressor_(suppressor) {}

void ProcessingStage::run() {
    running_.store(true, std::memory_order_release);

    while (running_.load(std::memory_order_acquire)) {
        if (!pumpOnce()) {
            std::this_thread::yield(); // input empty — nothing to process yet
        }
    }
}

bool ProcessingStage::pumpOnce() {
    AudioFrame in;
    if (!input_.read(in)) {
        return false; // nothing to process
    }
    AudioFrame out = suppressor_.process(in);
    if (output_.write(out)) {
        framesProcessed_.fetch_add(1, std::memory_order_relaxed);
    } else {
        framesDropped_.fetch_add(1, std::memory_order_relaxed); // output buffer full
    }
    return true;
}

void ProcessingStage::requestStop() {
    running_.store(false, std::memory_order_release);
}
