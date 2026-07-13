#include "suppressor.h"

#include <cmath>

namespace {
constexpr int kFrame = 480;             // AudioFrame size (half the analysis window)
constexpr double kPi = 3.14159265358979323846;
}  // namespace

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
    // Build the 960-sample window: [previous frame][current frame], then remember the current
    // frame as the "previous" for the next call.
    float windowBuf[fft::kWindowSize];
    for (int i = 0; i < kFrame; ++i) {
        windowBuf[i] = previousInputFrame_[i];
        windowBuf[kFrame + i] = in[i];
        previousInputFrame_[i] = in[i];
    }

    // Window and transform to the frequency domain.
    applyWindow(windowBuf);
    fft::Cpx spectrum[fft::kFreqSize];
    fft_.forward(windowBuf, spectrum);

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
