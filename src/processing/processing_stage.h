#pragma once

#include "../core/audio_frame.h"
#include "../core/ring_buffer.h"
#include "../suppressor/suppressor.h"

#include <atomic>
#include <cstdint>

// Drives one pipeline stage: drain `input`, run each frame through `suppressor`, push the
// result to `output`. Passive component like WasapiCapture — it owns the hot-loop function
// (run()) but NOT the std::thread; main.cpp spawns a thread on run() and calls requestStop()
// to unblock it.
//
// Both ring buffers are SPSC: this stage is the sole consumer of `input` and the sole
// producer of `output`.
class ProcessingStage {
public:
    ProcessingStage(RingBuffer<AudioFrame>& input,
                    RingBuffer<AudioFrame>& output,
                    Suppressor& suppressor);

    ProcessingStage(const ProcessingStage&) = delete;
    ProcessingStage& operator=(const ProcessingStage&) = delete;

    void run();          // loop until requestStop(): read input → process → write output
    // Flip the stop flag; the loop observes it within one yield cycle. Safe to call before the
    // run() thread has started — run() only reads the flag, so an early stop makes run() return
    // immediately rather than being overwritten. One-shot per run cycle.
    void requestStop();

    // One iteration of run()'s loop: read a frame from input, suppress it, write to output.
    // Returns true if a frame was read (a full output counts as a drop, still "did work");
    // false if input was empty. Exposed for unit testing the flow without a thread.
    bool pumpOnce();

    uint64_t framesProcessed() const noexcept {
        return framesProcessed_.load(std::memory_order_relaxed);
    }
    uint64_t framesDropped() const noexcept {
        return framesDropped_.load(std::memory_order_relaxed);
    }

private:
    RingBuffer<AudioFrame>& input_;
    RingBuffer<AudioFrame>& output_;
    Suppressor& suppressor_;
    // Written only by requestStop(), only read by run() — so a stop racing ahead of the thread
    // entering run() cannot be clobbered.
    std::atomic<bool> stopRequested_{false};
    std::atomic<uint64_t> framesProcessed_{0};
    std::atomic<uint64_t> framesDropped_{0};
};
