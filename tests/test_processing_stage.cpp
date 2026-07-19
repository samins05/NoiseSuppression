#include "../src/processing/processing_stage.h"
#include "../src/suppressor/suppressor.h"
#include "../src/core/ring_buffer.h"
#include "../src/core/audio_frame.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>

namespace {

AudioFrame makeFrame(float v) {
    AudioFrame f;
    f.fill(v);
    return f;
}

// A single frame flows Raw -> Clean and the counters reflect it. The Suppressor is no longer
// identity (it's the STFT scaffold), so we check the output against a reference run of the same
// transform rather than a hardcoded value — this test is about the pump-loop wiring, not the DSP.
void test_single_frame_flows() {
    RingBuffer<AudioFrame> raw(8), clean(8);
    Suppressor sup;
    ProcessingStage ps(raw, clean, sup);

    assert(raw.write(makeFrame(1.0f)));
    assert(ps.pumpOnce());
    assert(ps.framesProcessed() == 1);
    assert(ps.framesDropped() == 0);

    Suppressor ref;
    AudioFrame expected = ref.process(makeFrame(1.0f));

    AudioFrame f;
    assert(clean.read(f));
    assert(f == expected); // pump ran the suppressor and forwarded its exact output
    assert(!raw.read(f));  // input was consumed
}

// An empty input buffer is a no-op: pumpOnce returns false, produces no output, counters stay 0.
void test_empty_input_is_noop() {
    RingBuffer<AudioFrame> raw(8), clean(8);
    Suppressor sup;
    ProcessingStage ps(raw, clean, sup);

    assert(!ps.pumpOnce()); // nothing to read
    assert(ps.framesProcessed() == 0);
    assert(ps.framesDropped() == 0);

    AudioFrame f;
    assert(!clean.read(f)); // nothing produced
}

// Multiple frames pass through in order, one output per input. Verified against a reference
// Suppressor fed the same frames in the same order (deterministic transform -> bit-identical).
void test_multiple_frames_in_order() {
    RingBuffer<AudioFrame> raw(8), clean(8);
    Suppressor sup;
    ProcessingStage ps(raw, clean, sup);

    AudioFrame a = makeFrame(1.0f), b = makeFrame(2.0f), c = makeFrame(3.0f);
    assert(raw.write(a));
    assert(raw.write(b));
    assert(raw.write(c));

    for (int i = 0; i < 3; ++i) assert(ps.pumpOnce());
    assert(ps.framesProcessed() == 3);

    Suppressor ref;
    AudioFrame ea = ref.process(a), eb = ref.process(b), ec = ref.process(c);

    AudioFrame f;
    assert(clean.read(f)); assert(f == ea);
    assert(clean.read(f)); assert(f == eb);
    assert(clean.read(f)); assert(f == ec);
}

// When Clean is full, the extra frame is dropped and counted (input still consumed).
void test_drop_when_clean_full() {
    RingBuffer<AudioFrame> raw(8), clean(2); // clean holds 2 frames
    Suppressor sup;
    ProcessingStage ps(raw, clean, sup);

    for (int i = 0; i < 3; ++i) assert(raw.write(makeFrame(static_cast<float>(i))));
    for (int i = 0; i < 3; ++i) assert(ps.pumpOnce()); // each reads a frame -> true

    assert(ps.framesProcessed() == 2);
    assert(ps.framesDropped() == 1);
}

// Regression test for the startup race: a requestStop() that lands BEFORE the thread enters run()
// must still be honored. The old code did `running_ = true` at the top of run(), which clobbered
// the stop and left the loop spinning forever (join would hang).
void test_stop_before_run_is_honored() {
    RingBuffer<AudioFrame> raw(8), clean(8);
    Suppressor sup;
    ProcessingStage ps(raw, clean, sup);

    ps.requestStop(); // stop arrives first — before run() is ever entered

    auto fut = std::async(std::launch::async, [&ps] { ps.run(); });
    if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        std::fprintf(stderr, "run() never returned after an early requestStop() — race regressed\n");
        std::abort(); // abort rather than return: ~future would block forever on the stuck thread
    }
}

} // namespace

int main() {
    test_single_frame_flows();
    test_empty_input_is_noop();
    test_multiple_frames_in_order();
    test_drop_when_clean_full();
    test_stop_before_run_is_honored();

    std::cout << "ProcessingStage test passed: 5 cases (flow, empty-input, order, overflow, "
                 "stop-before-run)." << std::endl;
    return 0;
}
