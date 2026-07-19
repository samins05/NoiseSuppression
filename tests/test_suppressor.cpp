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

// ---- signal generators (all pure functions of absolute sample index, so frames join seamlessly)

// Continuous multi-tone: the easy, smooth case.
float toneAt(int n) {
    double t = (double)n;
    double v = 0.5 * std::sin(2.0 * kPi * 3.0 * t / kFrame)
             + 0.3 * std::sin(2.0 * kPi * 7.5 * t / kFrame + 0.7)
             + 0.15 * std::sin(2.0 * kPi * 19.0 * t / kFrame + 1.9);
    return (float)v;
}

// White noise: spectrally flat, so every FFT bin is exercised rather than a handful of tones.
float noiseAt(int n) {
    unsigned s = (unsigned)n * 1664525u + 1013904223u;
    s ^= s >> 16;
    s *= 2246822519u;
    s ^= s >> 13;
    return (float)((double)(s >> 8) / (double)(1u << 24)) * 2.0f - 1.0f;
}

// Isolated clicks plus an abrupt on/off burst — hard discontinuities are the worst case for
// windowed overlap-add, where a seam bug shows up as ringing around the transient.
float transientAt(int n) {
    if (n % 1000 == 0) return 0.9f;
    if (n >= 4800 && n < 5280) return 0.6f;
    return 0.0f;
}

// Constant signal: exercises bin 0 (DC) and catches slow drift.
float dcAt(int) { return 0.5f; }

// ---- helpers

template <typename Gen>
std::vector<AudioFrame> makeFrames(int frames, Gen gen) {
    std::vector<AudioFrame> v(frames);
    for (int f = 0; f < frames; ++f) {
        for (int i = 0; i < kFrame; ++i) v[f][i] = gen(f * kFrame + i);
    }
    return v;
}

// The Suppressor high-passes every frame before analysis, and synthesis reconstructs from that
// filtered spectrum — so the thing it should reproduce is the *high-passed* input, not the raw
// input. Build that reference with an independent HighPassFilter (same code, fresh state).
std::vector<AudioFrame> highPassReference(const std::vector<AudioFrame>& inputs) {
    HighPassFilter hp;
    std::vector<AudioFrame> refs(inputs.size());
    for (size_t f = 0; f < inputs.size(); ++f) {
        hp.process(inputs[f].data(), refs[f].data(), kFrame);
    }
    return refs;
}

// Push frames through one Suppressor and return {signalEnergy, errorEnergy} against the
// high-passed reference. Output frame k is compared against reference frame k-1 (the STFT's
// inherent one-frame delay). `measureFrom` lets a test ignore early frames and score only the
// tail, which is how we detect long-run drift.
struct Residual {
    double signalEnergy;
    double errorEnergy;
};

Residual reconstructionResidual(const std::vector<AudioFrame>& inputs, int measureFrom = 1) {
    Suppressor sup;
    std::vector<AudioFrame> outputs(inputs.size());
    for (size_t f = 0; f < inputs.size(); ++f) outputs[f] = sup.process(inputs[f]);
    const std::vector<AudioFrame> refs = highPassReference(inputs);

    Residual r{0.0, 0.0};
    for (size_t f = (size_t)std::max(1, measureFrom); f < inputs.size(); ++f) {
        for (int i = 0; i < kFrame; ++i) {
            double s = refs[f - 1][i];
            double e = (double)outputs[f][i] - s;
            r.signalEnergy += s * s;
            r.errorEnergy += e * e;
        }
    }
    return r;
}

double reconstructionSnr(const std::vector<AudioFrame>& inputs, int measureFrom = 1) {
    Residual r = reconstructionResidual(inputs, measureFrom);
    return 10.0 * std::log10(r.signalEnergy / r.errorEnergy);
}

void assertSnrAbove(const char* name, double snr, double floorDb) {
    if (!(snr > floorDb)) std::fprintf(stderr, "%s: SNR %.2f dB (want > %.1f)\n", name, snr, floorDb);
    assert(snr > floorDb);
}

// ---- cases

// With gains stubbed at 1.0 the scaffold must reconstruct the input exactly, one frame delayed.
void test_reconstruction_snr() {
    assertSnrAbove("tone", reconstructionSnr(makeFrames(20, toneAt)), 100.0);
}

// Flat-spectrum input: no bin gets a free pass.
void test_reconstruction_noise() {
    assertSnrAbove("noise", reconstructionSnr(makeFrames(20, noiseAt)), 100.0);
}

// Clicks and abrupt bursts must survive the window/overlap-add without smearing.
void test_reconstruction_transient() {
    assertSnrAbove("transient", reconstructionSnr(makeFrames(20, transientAt)), 100.0);
}

// DC is the degenerate case at bin 0 — and it is exactly what the high-pass is there to remove, so
// the reference decays toward silence and an SNR *ratio* stops being meaningful (near-zero signal
// energy in the denominator). Score absolute reconstruction error instead, and separately confirm
// the filter really is killing the DC.
void test_reconstruction_dc() {
    auto frames = makeFrames(20, dcAt);
    Residual r = reconstructionResidual(frames);

    const double rmsError = std::sqrt(r.errorEnergy / (19.0 * kFrame));
    if (!(rmsError < 1e-5)) std::fprintf(stderr, "dc: rms reconstruction error %.3e\n", rmsError);
    assert(rmsError < 1e-5);

    // Sanity: a constant 0.5 input should be substantially attenuated by the end of 20 frames.
    auto refs = highPassReference(frames);
    assert(std::fabs(refs.back()[kFrame - 1]) < 0.05f);
}

// Silence in must be silence out — guards against NaN/denormal creep through the transform.
void test_silence_stays_silent() {
    Suppressor sup;
    AudioFrame zero;
    zero.fill(0.0f);
    for (int f = 0; f < 5; ++f) {
        AudioFrame out = sup.process(zero);
        for (int i = 0; i < kFrame; ++i) {
            assert(std::isfinite(out[i]));
            assert(std::fabs(out[i]) < 1e-12f);
        }
    }
}

// Long run: score only the last 100 of 1000 frames. If previousInputFrame_/overlapTail_ accumulated
// error or drifted, the tail SNR would sag well below the fresh-start figure.
void test_long_run_stability() {
    auto frames = makeFrames(1000, toneAt);
    assertSnrAbove("long-run tail", reconstructionSnr(frames, 900), 100.0);
}

// The first output frame is silent: both overlap buffers start zeroed, so nothing has propagated.
void test_first_frame_silent() {
    Suppressor sup;
    AudioFrame in;
    for (int i = 0; i < kFrame; ++i) in[i] = toneAt(i);
    AudioFrame out = sup.process(in);
    for (int i = 0; i < kFrame; ++i) assert(std::fabs(out[i]) < 1e-6f);
}

}  // namespace

int main() {
    test_reconstruction_snr();
    test_reconstruction_noise();
    test_reconstruction_transient();
    test_reconstruction_dc();
    test_silence_stays_silent();
    test_long_run_stability();
    test_first_frame_silent();
    std::cout << "suppressor test passed: 7 cases (tone, noise, transient, dc, silence, long-run, "
                 "first-frame-silent)." << std::endl;
    return 0;
}
