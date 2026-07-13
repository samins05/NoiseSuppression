#pragma once

#include <array>

#include "../core/audio_frame.h"
#include "fft.h"

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

private:
    void applyWindow(float buf[fft::kWindowSize]) const;

    fft::Fft fft_;
    std::array<float, 480> windowTaper_{};         // Vorbis half-window taper (denoise.c:172-173)
    std::array<float, 480> previousInputFrame_{};  // last call's input frame = the 960 window's first half
    std::array<float, 480> overlapTail_{};         // leftover second half carried forward for overlap-add
};
