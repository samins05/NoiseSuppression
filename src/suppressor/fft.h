#pragma once

// 960-point real FFT/IFFT for the suppressor, wrapping the vendored KISS FFT (src/suppressor/kiss/).
// RNNoise analyzes 960-sample windows (two overlapped 480 frames); 960 = 4*4*4*5*3 is not a power
// of two, so this uses KISS's mixed-radix transform. This wrapper reproduces RNNoise's exact
// convention (see third_party rnnoise denoise.c forward_transform/inverse_transform) so band
// energies and gains match the golden oracle downstream.
//
// KISS's own headers are pulled in only by fft.cpp — consumers of this header stay clean and see
// just the fft::Cpx type below (bit-compatible with kiss_fft_cpx).

// Forward declaration: the KISS config type is opaque to consumers; defined in kiss_fft.h.
struct kiss_fft_state;

namespace fft {

constexpr int kWindowSize = 960;  // full analysis window (2 * 480-sample frames)
constexpr int kFreqSize = 481;    // N/2 + 1 unique bins for a real-valued input signal

// Complex bin. Same layout as KISS's kiss_fft_cpx ({float r; float i;}); fft.cpp static_asserts it.
struct Cpx {
    float r;
    float i;
};

// One instance owns one 960-point KISS config. Allocation happens once in the constructor
// (startup), satisfying the no-allocation-after-startup constraint. Single-threaded use.
class Fft {
public:
    Fft();
    ~Fft();

    Fft(const Fft&) = delete;
    Fft& operator=(const Fft&) = delete;

    // Real 960-sample window -> 481 complex bins (scaled by 1/N, matching KISS's forward scale).
    void forward(const float in[kWindowSize], Cpx out[kFreqSize]);

    // 481 complex bins -> real 960-sample window. Reconstructs the upper half by Hermitian
    // symmetry, then undoes the forward scale (xN + reversed index) to return the time signal.
    void inverse(const Cpx in[kFreqSize], float out[kWindowSize]);

private:
    kiss_fft_state* cfg_;
};

}  // namespace fft
