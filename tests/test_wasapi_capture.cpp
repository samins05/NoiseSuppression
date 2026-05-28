#include "../src/capture/wasapi_capture.h"
#include "../src/core/ring_buffer.h"

#include <cassert>
#include <iostream>
#include <vector>

namespace {

constexpr size_t kFrame = 480; // samples per AudioFrame

// Exactly one frame's worth of mono samples -> one frame, contents preserved.
void test_exact_frame() {
    RingBuffer<AudioFrame> buf(8);
    WasapiCapture cap(buf);

    std::vector<float> mono(kFrame);
    for (size_t i = 0; i < kFrame; ++i) mono[i] = static_cast<float>(i) * 0.001f;

    cap.ingestSamples(mono.data(), kFrame, 1, false);

    assert(cap.framesProduced() == 1);
    assert(cap.framesDropped() == 0);

    AudioFrame f;
    assert(buf.read(f));
    for (size_t i = 0; i < kFrame; ++i) assert(f[i] == mono[i]);
    assert(!buf.read(f)); // only one frame emitted
}

// Accumulator persists across calls (variable-size packets sum to one frame).
void test_accumulate_across_calls() {
    RingBuffer<AudioFrame> buf(8);
    WasapiCapture cap(buf);

    std::vector<float> first(300, 1.0f);
    std::vector<float> second(180, 2.0f);

    cap.ingestSamples(first.data(), 300, 1, false);
    assert(cap.framesProduced() == 0); // 300 < 480, nothing emitted yet

    cap.ingestSamples(second.data(), 180, 1, false);
    assert(cap.framesProduced() == 1); // 300 + 180 == 480

    AudioFrame f;
    assert(buf.read(f)); // buffer not empty
    for (size_t i = 0; i < 300; ++i)     assert(f[i] == 1.0f); // first 300 samples from first call
    for (size_t i = 300; i < kFrame; ++i) assert(f[i] == 2.0f); // next 180 samples from second call
}

// Stereo input is averaged to mono: (L + R) / 2.
void test_stereo_to_mono() {
    RingBuffer<AudioFrame> buf(8);
    WasapiCapture cap(buf);

    std::vector<float> stereo(kFrame * 2);
    for (size_t i = 0; i < kFrame; ++i) {
        stereo[2 * i]     = 1.0f; // left
        stereo[2 * i + 1] = 3.0f; // right
    }

    cap.ingestSamples(stereo.data(), kFrame, 2, false);

    assert(cap.framesProduced() == 1);
    AudioFrame f;
    assert(buf.read(f));
    for (size_t i = 0; i < kFrame; ++i) assert(f[i] == 2.0f); // (1+3)/2
}

// Silent flag writes zeros regardless of sample contents.
void test_silent_writes_zeros() {
    RingBuffer<AudioFrame> buf(8);
    WasapiCapture cap(buf);

    std::vector<float> noisy(kFrame, 9.0f);
    cap.ingestSamples(noisy.data(), kFrame, 1, true);

    assert(cap.framesProduced() == 1);
    AudioFrame f;
    assert(buf.read(f));
    for (size_t i = 0; i < kFrame; ++i) assert(f[i] == 0.0f);
}

// A chunk spanning multiple frames emits multiple frames in order.
void test_multiple_frames_one_call() {
    RingBuffer<AudioFrame> buf(8);
    WasapiCapture cap(buf);

    std::vector<float> mono(kFrame * 2);
    for (size_t i = 0; i < mono.size(); ++i) mono[i] = static_cast<float>(i) * 0.0001f;

    cap.ingestSamples(mono.data(), kFrame * 2, 1, false);

    assert(cap.framesProduced() == 2);
    AudioFrame f1, f2;
    assert(buf.read(f1));
    assert(buf.read(f2));
    for (size_t i = 0; i < kFrame; ++i) assert(f1[i] == mono[i]);
    for (size_t i = 0; i < kFrame; ++i) assert(f2[i] == mono[kFrame + i]);
}

// When the output buffer is full, extra frames are dropped and counted.
void test_drop_when_full() {
    RingBuffer<AudioFrame> buf(2); // capacity() == 2 usable frames
    WasapiCapture cap(buf);

    std::vector<float> mono(kFrame * 3, 1.0f); // three frames, no draining in between
    cap.ingestSamples(mono.data(), kFrame * 3, 1, false);

    assert(cap.framesProduced() == 2);
    assert(cap.framesDropped() == 1);
}

// A partial frame is held in the accumulator, not written prematurely.
void test_partial_leftover() {
    RingBuffer<AudioFrame> buf(8);
    WasapiCapture cap(buf);

    std::vector<float> mono(500, 1.0f); // 480 -> one frame, 20 left over
    cap.ingestSamples(mono.data(), 500, 1, false);

    assert(cap.framesProduced() == 1);
    assert(cap.framesDropped() == 0);
    AudioFrame f;
    assert(buf.read(f));
    assert(!buf.read(f)); // the leftover 20 samples are not a frame yet
}

} // namespace

int main() {
    test_exact_frame();
    test_accumulate_across_calls();
    test_stereo_to_mono();
    test_silent_writes_zeros();
    test_multiple_frames_one_call();
    test_drop_when_full();
    test_partial_leftover();

    std::cout << "WasapiCapture test passed: 7 cases (framing, mixing, silent, overflow)." << std::endl;
    return 0;
}
