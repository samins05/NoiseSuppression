#include "suppressor.h"

AudioFrame Suppressor::process(const AudioFrame& in) {
    return in; // passthrough — RNNoise DSP not yet implemented
}
