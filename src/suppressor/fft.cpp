#include "fft.h"

#include <cassert>
#include <cstddef>
#include <type_traits>

#include "kiss/kiss_fft.h"

namespace fft {

// Our Cpx must be layout-compatible with KISS's kiss_fft_cpx so we can reinterpret buffers of one
// as the other. Both are {float r; float i;} — verify at compile time.
static_assert(sizeof(Cpx) == sizeof(kiss_fft_cpx), "Cpx must match kiss_fft_cpx size");
static_assert(std::is_standard_layout<Cpx>::value, "Cpx must be standard-layout");
static_assert(offsetof(Cpx, r) == 0, "Cpx.r must be first");
static_assert(offsetof(Cpx, i) == sizeof(float), "Cpx.i must follow Cpx.r");

Fft::Fft() {
    // The single allocation, at startup. arch=0 selects the portable C path.
    cfg_ = rnn_fft_alloc_twiddles(kWindowSize, nullptr, nullptr, nullptr, 0);
    assert(cfg_ != nullptr && "KISS FFT config allocation failed");
}

Fft::~Fft() {
    rnn_fft_free(cfg_, 0);
}

void Fft::forward(const float in[kWindowSize], Cpx out[kFreqSize]) {
    // Load reals as complex (imaginary part zero), full 960-point complex FFT, keep the 481
    // unique bins. rnn_fft_c already applies the 1/N scale (kiss_fft.c: st->scale = 1.f/nfft).
    kiss_fft_cpx x[kWindowSize];
    kiss_fft_cpx y[kWindowSize];
    for (int i = 0; i < kWindowSize; ++i) {
        x[i].r = in[i];
        x[i].i = 0.0f;
    }
    rnn_fft_c(cfg_, x, y);
    for (int i = 0; i < kFreqSize; ++i) {
        out[i].r = y[i].r;
        out[i].i = y[i].i;
    }
}

void Fft::inverse(const Cpx in[kFreqSize], float out[kWindowSize]) {
    // Rebuild the full 960-bin spectrum from the 481 unique bins via conjugate (Hermitian)
    // symmetry, run the same forward FFT, then undo the forward 1/N scale: xN and reverse the
    // output index. Matches RNNoise's inverse_transform exactly.
    kiss_fft_cpx x[kWindowSize];
    kiss_fft_cpx y[kWindowSize];
    int i = 0;
    for (; i < kFreqSize; ++i) {
        x[i].r = in[i].r;
        x[i].i = in[i].i;
    }
    for (; i < kWindowSize; ++i) {
        x[i].r = x[kWindowSize - i].r;
        x[i].i = -x[kWindowSize - i].i;
    }
    rnn_fft_c(cfg_, x, y);
    out[0] = kWindowSize * y[0].r;
    for (i = 1; i < kWindowSize; ++i) {
        out[i] = kWindowSize * y[kWindowSize - i].r;
    }
}

}  // namespace fft
