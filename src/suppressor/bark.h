#pragma once

#include "fft.h"

// Bark-scale band analysis: collapse the 481 FFT bins into 22 perceptual bands.
//
// FFT gives us 481 separate frequency measurements, collapse these into 22 bands that our human ears can perceive. 
// For each band, compute: how much energy in it -> this allows us to form a "fingerprint" of our sound, this gets fed into the neural network
// ** Note: the GRU reasons about 22 numbers per frame, not 481.
//
// Ported from RNNoise v0.1.1 (denoise.c compute_band_energy / eband5ms). Must match the reference
// exactly — these energies feed the BFCCs and ultimately the pretrained weights.
namespace bark {

constexpr int kNumBands = 22;

// Band edges in FFT-bin units. The reference stores them in "5 ms bin" units and shifts left by
// FRAME_SIZE_SHIFT (2), giving bins 0, 4, 8, ... 400. Bands therefore cover bins 0..400 (0-20 kHz);
// bins 401..480 (20-24 kHz) are deliberately unused — RNNoise ignores that top octave.
constexpr int kBandEdges[kNumBands] = {
    0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 136, 160, 192, 240, 312, 400
};

// Energy per band, as a triangular (50%-overlapping) filterbank: each bin's |X|^2 is split between
// the two bands it straddles, weighted by how far into the band it sits. Bands 0 and 21 are then
// doubled to compensate for having only one neighbour to borrow from.
void computeBandEnergy(const fft::Cpx X[fft::kFreqSize], float Ex[kNumBands]);

}  // namespace bark
