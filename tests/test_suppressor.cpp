#include "../src/suppressor/suppressor.h"
#include "../src/core/audio_frame.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

namespace {

constexpr int kFrame = 480;
const double kPi = std::acos(-1.0);

// A continuous, deterministic multi-tone signal indexed by absolute sample number, so successive
// 480-sample frames join seamlessly (no discontinuity the STFT would have to reconstruct across).
float sampleAt(int n) {
    double t = (double)n;
    double v = 0.5 * std::sin(2.0 * kPi * 3.0 * t / kFrame)
             + 0.3 * std::sin(2.0 * kPi * 7.5 * t / kFrame + 0.7)
             + 0.15 * std::sin(2.0 * kPi * 19.0 * t / kFrame + 1.9);
    return (float)v;
}

// With gains stubbed at 1.0, the scaffold must reconstruct the input exactly, delayed one frame:
// output frame k equals input frame k-1. Measure reconstruction SNR over the aligned region.
void test_reconstruction_snr() {
    Suppressor sup;
    const int frames = 20;

    std::vector<AudioFrame> inputs(frames), outputs(frames);
    for (int f = 0; f < frames; ++f) {
        for (int i = 0; i < kFrame; ++i) inputs[f][i] = sampleAt(f * kFrame + i);
        outputs[f] = sup.process(inputs[f]);
    }

    // Compare output frame k against input frame k-1 (skip frame 0: nothing has propagated yet).
    double signalEnergy = 0.0, errorEnergy = 0.0;
    for (int f = 1; f < frames; ++f) {
        for (int i = 0; i < kFrame; ++i) {
            double s = inputs[f - 1][i];
            double e = (double)outputs[f][i] - s;
            signalEnergy += s * s;
            errorEnergy += e * e;
        }
    }

    double snr = 10.0 * std::log10(signalEnergy / errorEnergy);
    if (snr <= 100.0) std::fprintf(stderr, "reconstruction SNR too low: %.2f dB\n", snr);
    assert(snr > 100.0);
}

// The first output frame is silent: both overlap buffers start zeroed, so nothing has propagated.
void test_first_frame_silent() {
    Suppressor sup;
    AudioFrame in;
    for (int i = 0; i < kFrame; ++i) in[i] = sampleAt(i);
    AudioFrame out = sup.process(in);
    for (int i = 0; i < kFrame; ++i) assert(std::fabs(out[i]) < 1e-6f);
}

}  // namespace

int main() {
    test_reconstruction_snr();
    test_first_frame_silent();
    std::cout << "suppressor test passed: 2 cases (reconstruction-snr, first-frame-silent)." << std::endl;
    return 0;
}
