#pragma once

#include "../core/audio_frame.h"

// Pure audio transform: one frame in, one clean frame out. No threads, no ring buffers —
// the ProcessingStage owns the loop and feeds frames through here.
//
// This first slice is passthrough. The real pipeline (FFT → Bark → feature extraction →
// GRU → gain → IFFT → overlap-add) will replace the body of process() later. process() is a
// non-const member because that pipeline carries state between frames (GRU hidden state and
// the overlap-add tail), which must persist across calls and never reset mid-session.
class Suppressor {
public:
    AudioFrame process(const AudioFrame& in);
};
