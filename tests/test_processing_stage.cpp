#include "../src/processing/processing_stage.h"
#include "../src/suppressor/suppressor.h"
#include "../src/core/ring_buffer.h"
#include "../src/core/audio_frame.h"

#include <cassert>
#include <iostream>

namespace {

AudioFrame makeFrame(float v) {
    AudioFrame f;
    f.fill(v);
    return f;
}

// A single frame flows Raw -> Clean and the counters reflect it.
void test_single_frame_flows() {
    RingBuffer<AudioFrame> raw(8), clean(8);
    Suppressor sup;
    ProcessingStage ps(raw, clean, sup);

    assert(raw.write(makeFrame(1.0f)));
    assert(ps.pumpOnce());
    assert(ps.framesProcessed() == 1);
    assert(ps.framesDropped() == 0);

    AudioFrame f;
    assert(clean.read(f));
    for (float x : f) assert(x == 1.0f);
    assert(!raw.read(f)); // input was consumed
}

// Passthrough suppressor preserves the frame exactly.
void test_passthrough_identity() {
    RingBuffer<AudioFrame> raw(8), clean(8);
    Suppressor sup;
    ProcessingStage ps(raw, clean, sup);

    AudioFrame in;
    for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(i) * 0.01f;
    assert(raw.write(in));

    assert(ps.pumpOnce());
    AudioFrame out;
    assert(clean.read(out));
    assert(out == in); // std::array compares element-wise
}

// Multiple frames preserve order.
void test_multiple_frames_in_order() {
    RingBuffer<AudioFrame> raw(8), clean(8);
    Suppressor sup;
    ProcessingStage ps(raw, clean, sup);

    assert(raw.write(makeFrame(1.0f)));
    assert(raw.write(makeFrame(2.0f)));
    assert(raw.write(makeFrame(3.0f)));

    for (int i = 0; i < 3; ++i) assert(ps.pumpOnce());
    assert(ps.framesProcessed() == 3);

    AudioFrame f;
    assert(clean.read(f)); assert(f[0] == 1.0f);
    assert(clean.read(f)); assert(f[0] == 2.0f);
    assert(clean.read(f)); assert(f[0] == 3.0f);
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

} // namespace

int main() {
    test_single_frame_flows();
    test_passthrough_identity();
    test_multiple_frames_in_order();
    test_drop_when_clean_full();

    std::cout << "ProcessingStage test passed: 4 cases (flow, identity, order, overflow)." << std::endl;
    return 0;
}
