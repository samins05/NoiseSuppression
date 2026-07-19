#include "../src/suppressor/bark.h"
#include "../src/suppressor/fft.h"
#include "../src/suppressor/suppressor.h"
#include "../src/core/audio_frame.h"
#include "wav_io.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kFrame = 480;
constexpr int kBands = bark::kNumBands;
const double kPi = std::acos(-1.0);

// ---------------------------------------------------------------- hermetic checks

// Every bin's energy is split (1-frac)/frac across two adjacent bands, so the weights sum to 1 and
// total band energy equals total bin energy — except bands 0 and 21, which are doubled. Backing
// that doubling out must recover the raw spectrum energy exactly.
void test_energy_conservation() {
    fft::Cpx X[fft::kFreqSize] = {};
    unsigned s = 22222u;
    for (int k = 0; k < fft::kFreqSize; ++k) {
        s = s * 1664525u + 1013904223u;
        X[k].r = (float)((double)(s >> 8) / (double)(1u << 24)) - 0.5f;
        s = s * 1664525u + 1013904223u;
        X[k].i = (float)((double)(s >> 8) / (double)(1u << 24)) - 0.5f;
    }

    float Ex[kBands];
    bark::computeBandEnergy(X, Ex);

    double banded = 0.0;
    for (int i = 0; i < kBands; ++i) banded += Ex[i];
    banded -= 0.5 * Ex[0] + 0.5 * Ex[kBands - 1]; // undo the edge doubling

    // Only bins inside the band range participate (0 .. last edge).
    double raw = 0.0;
    for (int k = 0; k < bark::kBandEdges[kBands - 1]; ++k) {
        raw += (double)X[k].r * X[k].r + (double)X[k].i * X[k].i;
    }

    assert(std::fabs(banded - raw) / raw < 1e-5);
}

// The 22 bands span bins 0..400; bins above the last edge are deliberately ignored (20-24 kHz).
void test_band_layout_and_upper_bins_ignored() {
    assert(bark::kBandEdges[0] == 0);
    assert(bark::kBandEdges[kBands - 1] == 400);
    for (int i = 1; i < kBands; ++i) assert(bark::kBandEdges[i] > bark::kBandEdges[i - 1]);

    // Energy placed only above the last edge must not register in any band.
    fft::Cpx X[fft::kFreqSize] = {};
    for (int k = bark::kBandEdges[kBands - 1]; k < fft::kFreqSize; ++k) X[k].r = 10.0f;

    float Ex[kBands];
    bark::computeBandEnergy(X, Ex);
    for (int i = 0; i < kBands; ++i) assert(Ex[i] == 0.0f);
}

void test_zero_spectrum() {
    fft::Cpx X[fft::kFreqSize] = {};
    float Ex[kBands];
    bark::computeBandEnergy(X, Ex);
    for (int i = 0; i < kBands; ++i) assert(Ex[i] == 0.0f);
}

// A single bin lands in the two bands it straddles, split by how far into its band it sits, and
// nowhere else.
void test_single_bin_spike() {
    const int band = 5;                        // bins 20..23
    const int start = bark::kBandEdges[band];
    const int size = bark::kBandEdges[band + 1] - start;
    const int offset = 2;                      // 2/4 of the way in -> even split

    fft::Cpx X[fft::kFreqSize] = {};
    X[start + offset].r = 2.0f;                // energy = 4
    const float energy = 4.0f;
    const float frac = (float)offset / size;

    float Ex[kBands];
    bark::computeBandEnergy(X, Ex);

    assert(std::fabs(Ex[band] - (1.0f - frac) * energy) < 1e-5f);
    assert(std::fabs(Ex[band + 1] - frac * energy) < 1e-5f);
    for (int i = 0; i < kBands; ++i) {
        if (i != band && i != band + 1) assert(Ex[i] == 0.0f);
    }
}

// ---------------------------------------------------------------- oracle diff (opt-in)

// Deterministic "noisy speech"-ish signal: a few voiced harmonics plus broadband noise, so the
// band energies span a wide dynamic range rather than sitting in a couple of bins.
float genSample(int n) {
    double t = (double)n / 48000.0;
    double voiced = 0.40 * std::sin(2 * kPi * 140.0 * t)     // ~pitch
                  + 0.22 * std::sin(2 * kPi * 420.0 * t)
                  + 0.12 * std::sin(2 * kPi * 980.0 * t)
                  + 0.06 * std::sin(2 * kPi * 2600.0 * t);
    voiced *= 0.6 + 0.4 * std::sin(2 * kPi * 3.0 * t);        // slow amplitude envelope
    unsigned s = (unsigned)n * 1664525u + 1013904223u;
    s ^= s >> 16; s *= 2246822519u; s ^= s >> 13;
    double noise = ((double)(s >> 8) / (double)(1u << 24) - 0.5) * 0.30;
    return (float)(voiced + noise);
}

int writeGeneratedInput(const char* path) {
    std::vector<float> samples(kFrame * 300); // 3 s
    for (size_t i = 0; i < samples.size(); ++i) samples[i] = genSample((int)i);
    if (!wav_io::writeRawPcm16(path, samples)) {
        std::fprintf(stderr, "failed to write %s\n", path);
        return 1;
    }
    std::printf("wrote %s (%zu samples, %zu frames)\n", path, samples.size(),
                samples.size() / kFrame);
    return 0;
}

// Parse the oracle's per-frame "Ex <22 floats>" lines (see tools/oracle/README.md).
std::vector<std::vector<float>> parseOracleEx(const std::string& dumpPath) {
    std::vector<std::vector<float>> frames;
    std::ifstream in(dumpPath);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("Ex ", 0) != 0) continue;
        std::istringstream iss(line.substr(3));
        std::vector<float> row;
        float v;
        while (iss >> v) row.push_back(v);
        if ((int)row.size() == kBands) frames.push_back(std::move(row));
    }
    return frames;
}

// Run the same PCM the oracle saw through our Suppressor and compare Ex frame by frame.
// Scale note: the oracle works on int16-magnitude floats (rnnoise_demo.c does `x[i] = tmp[i]`,
// no division), while wav_io hands us [-1,1]. Multiplying by 32768 is exact in float.
void test_oracle_ex_diff() {
    const char* dumpPath = std::getenv("ORACLE_DUMP");
    const char* inputPath = std::getenv("ORACLE_INPUT");
    if (!dumpPath || !inputPath) {
        std::cout << "  oracle diff skipped (set ORACLE_DUMP and ORACLE_INPUT to enable)\n";
        return;
    }

    std::vector<float> samples;
    if (!wav_io::readRawPcm16(inputPath, samples)) {
        std::fprintf(stderr, "could not read ORACLE_INPUT=%s\n", inputPath);
        assert(false);
    }
    std::vector<std::vector<float>> expected = parseOracleEx(dumpPath);
    assert(!expected.empty() && "no Ex lines parsed from ORACLE_DUMP");

    Suppressor sup;
    const size_t frames = std::min(samples.size() / kFrame, expected.size());
    double worstRel = 0.0;
    int worstFrame = -1, worstBand = -1;

    for (size_t f = 0; f < frames; ++f) {
        AudioFrame in;
        for (int i = 0; i < kFrame; ++i) in[i] = samples[f * kFrame + i] * 32768.0f;
        sup.process(in);
        const std::array<float, kBands>& got = sup.lastBandEnergies();

        for (int b = 0; b < kBands; ++b) {
            const double a = got[b], e = expected[f][b];
            const double denom = std::max({std::fabs(a), std::fabs(e), 1e-6});
            const double rel = std::fabs(a - e) / denom;
            if (rel > worstRel) { worstRel = rel; worstFrame = (int)f; worstBand = b; }
        }
    }

    if (worstFrame < 0) {
        std::printf("  oracle diff: %zu frames, bit-exact match on all %d bands\n", frames, kBands);
    } else {
        std::printf("  oracle diff: %zu frames, worst relative error %.3e (frame %d, band %d)\n",
                    frames, worstRel, worstFrame, worstBand);
    }
    assert(worstRel < 1e-3);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--gen-input") == 0) {
        return writeGeneratedInput(argv[2]);
    }

    test_energy_conservation();
    test_band_layout_and_upper_bins_ignored();
    test_zero_spectrum();
    test_single_bin_spike();
    test_oracle_ex_diff();

    std::cout << "bark test passed: 4 cases (energy-conservation, layout, zero, spike) + oracle diff."
              << std::endl;
    return 0;
}
