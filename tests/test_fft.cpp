#include "../src/suppressor/fft.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>

namespace {

constexpr int N = fft::kWindowSize;   // 960
constexpr int F = fft::kFreqSize;     // 481
const double kPi = std::acos(-1.0);

// Deterministic pseudo-random signal in ~[-1, 1] (fixed seed -> reproducible test).
void fillPseudoRandom(float* x, unsigned seed) {
    unsigned s = seed;
    for (int i = 0; i < N; ++i) {
        s = s * 1664525u + 1013904223u;         // LCG
        x[i] = (float)((double)(s >> 8) / (double)(1u << 24)) * 2.0f - 1.0f;
    }
}

// forward() of a unit impulse is flat: every bin equals 1/N, imaginary part zero.
// (The forward transform carries KISS's 1/N scale, so x[0]=1 spreads to 1/N across all bins.)
void test_impulse_flat() {
    fft::Fft f;
    float in[N] = {0};
    in[0] = 1.0f;
    fft::Cpx out[F];
    f.forward(in, out);

    const float expected = 1.0f / N;
    for (int k = 0; k < F; ++k) {
        assert(std::fabs(out[k].r - expected) < 1e-5f);
        assert(std::fabs(out[k].i) < 1e-5f);
    }
}

// A pure cosine at an exact integer bin k0 concentrates all energy in bin k0.
// DFT of cos(2*pi*k0*n/N) is N/2 at bin k0 (and its mirror N-k0); with the 1/N scale that is 0.5.
void test_sine_single_bin() {
    fft::Fft f;
    const int k0 = 10;
    float in[N];
    for (int n = 0; n < N; ++n) in[n] = (float)std::cos(2.0 * kPi * k0 * n / N);
    fft::Cpx out[F];
    f.forward(in, out);

    for (int k = 0; k < F; ++k) {
        if (k == k0) {
            assert(std::fabs(out[k].r - 0.5f) < 1e-4f);
            assert(std::fabs(out[k].i) < 1e-4f);
        } else {
            assert(std::hypot(out[k].r, out[k].i) < 1e-4f);
        }
    }
}

// inverse(forward(x)) == x (within float epsilon). The load-bearing round-trip identity.
void test_roundtrip() {
    fft::Fft f;
    float in[N];
    fillPseudoRandom(in, 12345u);
    fft::Cpx spec[F];
    float out[N];
    f.forward(in, spec);
    f.inverse(spec, out);

    for (int n = 0; n < N; ++n) assert(std::fabs(out[n] - in[n]) < 1e-3f);
}

// Parseval: time-domain energy equals frequency-domain energy, accounting for the 1/N scale and
// the fact that we keep only the 481 unique bins (bins 1..479 each stand in for a mirror twin).
void test_parseval() {
    fft::Fft f;
    float in[N];
    fillPseudoRandom(in, 6789u);
    fft::Cpx spec[F];
    f.forward(in, spec);

    double timeEnergy = 0.0;
    for (int n = 0; n < N; ++n) timeEnergy += (double)in[n] * in[n];

    // sum over full spectrum |X|^2, using symmetry: DC and Nyquist counted once, the rest twice.
    double freqEnergy = (double)spec[0].r * spec[0].r + (double)spec[0].i * spec[0].i;
    freqEnergy += (double)spec[N / 2].r * spec[N / 2].r + (double)spec[N / 2].i * spec[N / 2].i;
    for (int k = 1; k < N / 2; ++k) {
        freqEnergy += 2.0 * ((double)spec[k].r * spec[k].r + (double)spec[k].i * spec[k].i);
    }
    freqEnergy *= (double)N;  // undo the (1/N)^2 in the bins and the 1/N in Parseval => xN

    assert(std::fabs(timeEnergy - freqEnergy) / timeEnergy < 1e-3);
}

// forward() matches a slow, obviously-correct O(N^2) DFT (computed in double) within epsilon.
void test_vs_naive_dft() {
    fft::Fft f;
    float in[N];
    fillPseudoRandom(in, 424242u);
    fft::Cpx out[F];
    f.forward(in, out);

    for (int k = 0; k < F; ++k) {
        double re = 0.0, im = 0.0;
        for (int n = 0; n < N; ++n) {
            double ang = -2.0 * kPi * k * n / N;
            re += in[n] * std::cos(ang);
            im += in[n] * std::sin(ang);
        }
        re /= N;  // match KISS's 1/N forward scale
        im /= N;
        assert(std::fabs(out[k].r - re) < 1e-4);
        assert(std::fabs(out[k].i - im) < 1e-4);
    }
}

}  // namespace

int main() {
    test_impulse_flat();
    test_sine_single_bin();
    test_roundtrip();
    test_parseval();
    test_vs_naive_dft();
    std::cout << "fft test passed: 5 cases (impulse, sine, roundtrip, parseval, vs-dft)." << std::endl;
    return 0;
}
