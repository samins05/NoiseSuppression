#include "wav_io.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

// Build a signal whose samples are exactly representable as int16/32768, so write->read
// is bit-exact and any mismatch is a real bug (not quantization noise).
std::vector<float> makeExactSignal(size_t n) {
    std::vector<float> s(n);
    for (size_t i = 0; i < n; ++i) {
        int16_t k = static_cast<int16_t>((static_cast<int>(i) * 37) % 65536 - 32768);
        s[i] = wav_io::pcm16ToFloat(k);
    }
    return s;
}

std::string tmpPath(const char* name) {
    const char* dir = std::getenv("WAV_TEST_DIR");
    std::string base = dir ? dir : ".";
    return base + "/" + name;
}

// WAV round-trip preserves samples exactly and reports the sample rate back.
void test_wav_roundtrip() {
    auto in = makeExactSignal(4801);
    std::string path = tmpPath("rt.wav");
    assert(wav_io::writeWavPcm16(path, in, 48000));

    std::vector<float> out;
    uint32_t sr = 0;
    assert(wav_io::readWavPcm16(path, out, sr));
    assert(sr == 48000);
    assert(out.size() == in.size());
    for (size_t i = 0; i < in.size(); ++i) assert(out[i] == in[i]);
}

// Raw 16-bit PCM round-trip (the format the RNNoise oracle speaks).
void test_raw_roundtrip() {
    auto in = makeExactSignal(2000);
    std::string path = tmpPath("rt.pcm");
    assert(wav_io::writeRawPcm16(path, in));

    std::vector<float> out;
    assert(wav_io::readRawPcm16(path, out));
    assert(out.size() == in.size());
    for (size_t i = 0; i < in.size(); ++i) assert(out[i] == in[i]);
}

// The 'data' payload of our WAV equals the raw-PCM encoding of the same samples: proves
// the 44-byte header is correctly sized so oracle bridging (WAV<->raw) lines up.
void test_wav_data_matches_raw() {
    auto in = makeExactSignal(960);
    std::string wavPath = tmpPath("hdr.wav");
    std::string rawPath = tmpPath("hdr.pcm");
    assert(wav_io::writeWavPcm16(wavPath, in, 48000));
    assert(wav_io::writeRawPcm16(rawPath, in));

    std::FILE* w = std::fopen(wavPath.c_str(), "rb");
    std::FILE* r = std::fopen(rawPath.c_str(), "rb");
    assert(w && r);
    std::fseek(w, 44, SEEK_SET); // skip standard 44-byte PCM WAV header
    int a, b;
    do {
        a = std::fgetc(w);
        b = std::fgetc(r);
        assert(a == b);
    } while (a != EOF && b != EOF);
    std::fclose(w);
    std::fclose(r);
}

// Clamping: out-of-range floats saturate instead of wrapping.
void test_clamp() {
    assert(wav_io::floatToPcm16(2.0f)  == 32767);
    assert(wav_io::floatToPcm16(-2.0f) == -32768);
    assert(wav_io::floatToPcm16(0.0f)  == 0);
}

} // namespace

int main() {
    test_wav_roundtrip();
    test_raw_roundtrip();
    test_wav_data_matches_raw();
    test_clamp();
    std::cout << "wav_io test passed: 4 cases (wav rt, raw rt, header size, clamp)." << std::endl;
    return 0;
}
