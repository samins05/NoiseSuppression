#pragma once

#include <array>

// 10 ms of mono audio at 48 kHz — the frame size RNNoise requires downstream.
// Shared by capture, suppressor, and processing so none of them depend on each other's headers.
using AudioFrame = std::array<float, 480>;
