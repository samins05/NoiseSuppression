#include "suppressor.h"

#include <cmath>

namespace {
constexpr int kFrame = 480;             // AudioFrame size (half the analysis window)
constexpr double kPi = 3.14159265358979323846;

// High-pass (DC removal) biquad coefficients, verbatim from RNNoise (denoise.c a_hp/b_hp).
constexpr float kHpA[2] = {-1.99599f, 0.99600f};
constexpr float kHpB[2] = {-2.0f, 1.0f};
}  // namespace

void HighPassFilter::process(const float* in, float* out, int n) {
    // Direct-form transposed biquad. `xi` is latched before writing `out` so this stays correct
    // when out == in. The accumulate runs in double, matching the reference exactly — at these
    // coefficients (a pole at ~0.998) float rounding would drift audibly over a long session.
    for (int i = 0; i < n; ++i) {
        const float xi = in[i];
        const float yi = xi + mem[0];
        mem[0] = static_cast<float>(mem[1] + (kHpB[0] * (double)xi - kHpA[0] * (double)yi));
        mem[1] = static_cast<float>(kHpB[1] * (double)xi - kHpA[1] * (double)yi);
        out[i] = yi;
    }
}

Suppressor::Suppressor() {
    // Vorbis-style half-window taper (denoise.c:172-173). Applied on both the forward and inverse
    // side, so w^2 of overlapping windows sums to 1 -> exact reconstruction.
    for (int i = 0; i < kFrame; ++i) {
        double s = std::sin(0.5 * kPi * (i + 0.5) / kFrame);
        windowTaper_[i] = static_cast<float>(std::sin(0.5 * kPi * s * s));
    }
}

void Suppressor::applyWindow(float buf[fft::kWindowSize]) const {
    // Symmetric: the taper rises over the first 480 samples and mirrors down over the last 480.
    for (int i = 0; i < kFrame; ++i) {
        buf[i] *= windowTaper_[i];
        buf[fft::kWindowSize - 1 - i] *= windowTaper_[i];
    }
}

AudioFrame Suppressor::process(const AudioFrame& in) {
    // High-pass first: everything downstream (analysis memory, spectrum, and the audio we emit)
    // works on the filtered signal, matching the reference's biquad-then-analyse order.
    float filtered[kFrame];
    highPass_.process(in.data(), filtered, kFrame);

    // Build the 960-sample window: [previous frame][current frame], then remember the current
    // frame as the "previous" for the next call. Note these are the *filtered* samples.
    float windowBuf[fft::kWindowSize];
    for (int i = 0; i < kFrame; ++i) {
        windowBuf[i] = previousInputFrame_[i];
        windowBuf[kFrame + i] = filtered[i];
        previousInputFrame_[i] = filtered[i];
    }

    // Window and transform to the frequency domain.
    applyWindow(windowBuf);
    fft::Cpx spectrum[fft::kFreqSize];
    fft_.forward(windowBuf, spectrum);

    // Collapse the spectrum into 22 Bark-band energies. Slice 3 only *reads* this — it feeds the
    // BFCC/feature extraction in Slice 4 and the network in Slice 6.
    bark::computeBandEnergy(spectrum, lastBandEnergies_.data());

    // Gains = 1.0: the spectrum is left untouched. Real per-band gains land here in Slice 6.

    // Back to the time domain, then window again.
    fft_.inverse(spectrum, windowBuf);
    applyWindow(windowBuf);

    // Overlap-add: current window's first half + the previous window's saved tail. Save this
    // window's second half for the next call.
    AudioFrame out;
    for (int i = 0; i < kFrame; ++i) {
        out[i] = windowBuf[i] + overlapTail_[i];
        overlapTail_[i] = windowBuf[kFrame + i];
    }
    return out;
}
