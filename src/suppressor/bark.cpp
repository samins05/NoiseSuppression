#include "bark.h"

namespace bark {

void computeBandEnergy(const fft::Cpx X[fft::kFreqSize], float Ex[kNumBands]) {
    float sum[kNumBands] = {0};

    // Walk each band's bins once. `frac` ramps 0 -> 1 across the band, so a bin contributes
    // (1-frac) to the band it is leaving and frac to the band it is entering. Summed over the two
    // overlapping triangles, every bin contributes its full energy exactly once.
    for (int i = 0; i < kNumBands - 1; ++i) {
        const int bandStart = kBandEdges[i];
        const int bandSize = kBandEdges[i + 1] - kBandEdges[i];
        for (int j = 0; j < bandSize; ++j) {
            const float frac = static_cast<float>(j) / bandSize;
            const fft::Cpx& bin = X[bandStart + j];
            const float energy = bin.r * bin.r + bin.i * bin.i;
            sum[i] += (1.0f - frac) * energy;
            sum[i + 1] += frac * energy;
        }
    }

    // The first and last bands only ever receive one side of a triangle (nothing below band 0,
    // nothing above band 21), so they come out at half strength. Double them to compensate.
    sum[0] *= 2;
    sum[kNumBands - 1] *= 2;

    for (int i = 0; i < kNumBands; ++i) {
        Ex[i] = sum[i];
    }
}

}  // namespace bark
