#pragma once

// Header-only WAV / raw-PCM helper for OFFLINE TESTS ONLY. Not part of the real-time
// pipeline (it allocates freely and uses <cstdio>). It exists so the suppressor can be
// verified deterministically: read a known signal, run it through our DSP, write the
// result, and diff against the RNNoise oracle (which speaks raw 16-bit PCM).
//
// On-disk format is 16-bit signed mono PCM (WAV container or headerless raw). In memory
// audio is mono float. Scale convention (symmetric, exact round-trip for int16 sources):
//   encode: clamp(lrintf(s * 32768), -32768, +32767)
//   decode: i / 32768
// A float that is exactly k/32768 (k in [-32768, 32767]) survives write→read unchanged.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace wav_io {

inline int16_t floatToPcm16(float s) {
    long v = std::lrintf(s * 32768.0f);
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return static_cast<int16_t>(v);
}

inline float pcm16ToFloat(int16_t s) {
    return static_cast<float>(s) / 32768.0f;
}

// ---- little-endian scalar writers ----
inline void putU16(std::FILE* f, uint16_t v) {
    std::fputc(v & 0xFF, f);
    std::fputc((v >> 8) & 0xFF, f);
}
inline void putU32(std::FILE* f, uint32_t v) {
    std::fputc(v & 0xFF, f);
    std::fputc((v >> 8) & 0xFF, f);
    std::fputc((v >> 16) & 0xFF, f);
    std::fputc((v >> 24) & 0xFF, f);
}

// ---- raw headerless 16-bit PCM (what the RNNoise oracle reads/writes) ----
inline bool writeRawPcm16(const std::string& path, const std::vector<float>& samples) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    for (float s : samples) {
        int16_t v = floatToPcm16(s);
        std::fputc(v & 0xFF, f);
        std::fputc((v >> 8) & 0xFF, f);
    }
    std::fclose(f);
    return true;
}

inline bool readRawPcm16(const std::string& path, std::vector<float>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    int lo, hi;
    while ((lo = std::fgetc(f)) != EOF && (hi = std::fgetc(f)) != EOF) {
        int16_t v = static_cast<int16_t>((hi << 8) | lo);
        out.push_back(pcm16ToFloat(v));
    }
    std::fclose(f);
    return true;
}

// ---- WAV container (16-bit mono PCM) ----
inline bool writeWavPcm16(const std::string& path, const std::vector<float>& samples,
                          uint32_t sampleRate) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size()) * 2u;
    const uint16_t channels = 1, bitsPerSample = 16;
    const uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
    const uint16_t blockAlign = channels * (bitsPerSample / 8);

    std::fwrite("RIFF", 1, 4, f);
    putU32(f, 36 + dataBytes);       // file size minus the 8-byte RIFF header
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    putU32(f, 16);                   // fmt chunk size (PCM)
    putU16(f, 1);                    // audio format = PCM
    putU16(f, channels);
    putU32(f, sampleRate);
    putU32(f, byteRate);
    putU16(f, blockAlign);
    putU16(f, bitsPerSample);
    std::fwrite("data", 1, 4, f);
    putU32(f, dataBytes);
    for (float s : samples) {
        int16_t v = floatToPcm16(s);
        std::fputc(v & 0xFF, f);
        std::fputc((v >> 8) & 0xFF, f);
    }
    std::fclose(f);
    return true;
}

// Minimal WAV reader: seeks the 'data' chunk, assumes 16-bit PCM. Returns sample rate too.
inline bool readWavPcm16(const std::string& path, std::vector<float>& out,
                         uint32_t& sampleRate) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    auto rdU32 = [&](uint32_t& v) -> bool {
        int a = std::fgetc(f), b = std::fgetc(f), c = std::fgetc(f), d = std::fgetc(f);
        if (d == EOF) return false;
        v = (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
        return true;
    };
    auto rdU16 = [&](uint16_t& v) -> bool {
        int a = std::fgetc(f), b = std::fgetc(f);
        if (b == EOF) return false;
        v = (uint16_t)(a | (b << 8));
        return true;
    };
    char tag[4];
    uint32_t u32;
    if (std::fread(tag, 1, 4, f) != 4 || std::string(tag, 4) != "RIFF") { std::fclose(f); return false; }
    rdU32(u32); // RIFF size
    if (std::fread(tag, 1, 4, f) != 4 || std::string(tag, 4) != "WAVE") { std::fclose(f); return false; }

    sampleRate = 0;
    out.clear();
    // Walk chunks until we find 'data'; read sampleRate from 'fmt '.
    while (std::fread(tag, 1, 4, f) == 4) {
        uint32_t chunkSize = 0;
        if (!rdU32(chunkSize)) break;
        if (std::string(tag, 4) == "fmt ") {
            uint16_t fmt, ch, block, bits;
            uint32_t sr, br;
            rdU16(fmt); rdU16(ch); rdU32(sr); rdU32(br); rdU16(block); rdU16(bits);
            sampleRate = sr;
            for (uint32_t skipped = 16; skipped < chunkSize; ++skipped) std::fgetc(f); // any fmt extension
        } else if (std::string(tag, 4) == "data") {
            uint32_t n = chunkSize / 2;
            out.reserve(n);
            for (uint32_t i = 0; i < n; ++i) {
                int lo = std::fgetc(f), hi = std::fgetc(f);
                if (hi == EOF) break;
                out.push_back(pcm16ToFloat(static_cast<int16_t>((hi << 8) | lo)));
            }
            std::fclose(f);
            return true;
        } else {
            for (uint32_t i = 0; i < chunkSize; ++i) std::fgetc(f); // skip unknown chunk
        }
    }
    std::fclose(f);
    return false; // no data chunk
}

} // namespace wav_io
