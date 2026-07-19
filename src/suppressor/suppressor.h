#pragma once

#include <array>

#include "../core/audio_frame.h"
#include "bark.h"
#include "fft.h"

// RNNoise's DC-removal biquad, applied to every frame before analysis (denoise.c biquad + a_hp/b_hp).
// It is stateful across frames, and its output feeds both the analysis window and — since synthesis
// reconstructs from that same spectrum — the audio we emit. Exposed here (rather than hidden in the
// .cpp) so tests can build high-passed reference signals without duplicating the filter math.
struct HighPassFilter {
    // Filters n samples. Safe to call with out == in.
    void process(const float* in, float* out, int n);

    std::array<float, 2> mem{};
};

// Pure audio transform: one 480-sample frame in, one clean frame out. No threads, no ring buffers —
// the ProcessingStage owns the loop and feeds frames through here.
//
// This slice is the STFT scaffold with the noise-removal step stubbed to a no-op (gains = 1.0):
// each call assembles a 960-sample analysis window from the previous frame + the current frame,
// windows it, runs the FFT, leaves the spectrum untouched, inverse-FFTs, windows again, and
// overlap-adds with the previous window's tail to emit 480 samples. With gains at 1.0 this
// reconstructs the input exactly, delayed by one frame — which proves the window/overlap-add
// plumbing before any real gains (Slice 6) plug into step 5 of process().
//
// process() is non-const because it carries state between frames (the overlap buffers now; the
// GRU hidden state later), which must persist across calls and never reset mid-session.
class Suppressor {
public:
    Suppressor();

    AudioFrame process(const AudioFrame& in);

    // Band energies from the most recent process() call. Read-only view for tests and for the
    // feature extraction that lands in Slice 4.
    const std::array<float, bark::kNumBands>& lastBandEnergies() const { return lastBandEnergies_; }

private:
    void applyWindow(float buf[fft::kWindowSize]) const;

    fft::Fft fft_;
    HighPassFilter highPass_;
    std::array<float, bark::kNumBands> lastBandEnergies_{};
    std::array<float, 480> windowTaper_{};         // Vorbis half-window taper (denoise.c:172-173)
    std::array<float, 480> previousInputFrame_{};  // last call's input frame = the 960 window's first half
    std::array<float, 480> overlapTail_{};         // leftover second half carried forward for overlap-add
};
