#pragma once

#include "../core/audio_frame.h"
#include "../core/ring_buffer.h"

#include <atomic>
#include <cstdint>

// Forward-declare COM interfaces so callers don't need <mmdeviceapi.h> / <windows.h>.
struct IMMDeviceEnumerator;
struct IMMDevice;
struct IAudioClient;
struct IAudioCaptureClient;

// Passive WASAPI capture component. Owns the WASAPI handles and the hot-loop function;
// does NOT own the std::thread that runs it. The caller (main.cpp) spawns a thread that
// invokes run(), calls requestStop() to unblock it, then join()s the thread itself.

// Lifecycle contract: initialize() → caller spawns thread on run() → caller requestStop()s
// and joins → shutdown() (also runs from destructor). Destroying the object while a thread
// is still running run() is undefined behavior.
class WasapiCapture {
public:
    explicit WasapiCapture(RingBuffer<AudioFrame>& output);
    ~WasapiCapture();

    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    void initialize();   // COM init → device → AudioClient → event handle → CaptureClient. Throws on failure.
    void shutdown();     // Release WASAPI handles in reverse order. Idempotent.

    void run();          // Hot loop. Blocks until requestStop() is called.
    // Set the stop flag and wake the hot loop. Safe to call from any thread, and safe to call
    // *before* the run() thread has started — run() only ever reads the flag, so an early stop
    // makes run() return immediately instead of being overwritten. One-shot per run cycle.
    void requestStop();

    // Accumulate audio into fixed 480-sample AudioFrames, and write each completed frame to the
    // output ring buffer (bumping framesProduced/framesDropped) 
    // ** USED BY HOT LOOP** 
    void ingestSamples(const float* samples, uint32_t numFrames, uint32_t channels, bool silent);

    uint64_t framesProduced() const noexcept {
        return framesProduced_.load(std::memory_order_relaxed);
    }
    uint64_t framesDropped() const noexcept {
        return framesDropped_.load(std::memory_order_relaxed);
    }

private:
    RingBuffer<AudioFrame>& output_;
    // Split deliberately: stopRequested_ is written only by requestStop(), so a stop that races
    // ahead of the thread entering run() cannot be clobbered. threadActive_ tracks whether a
    // thread is currently inside run(), which is what the destructor asserts on.
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> threadActive_{false};
    std::atomic<uint64_t> framesProduced_{0};
    std::atomic<uint64_t> framesDropped_{0};

    // COM handles, owned by initialize() / shutdown(). Manual Release.
    IMMDeviceEnumerator* enumerator_    = nullptr;
    IMMDevice*           device_        = nullptr;
    IAudioClient*        audioClient_   = nullptr;
    IAudioCaptureClient* captureClient_ = nullptr;
    void*                eventHandle_   = nullptr; // HANDLE; void* keeps the header WinAPI-free.
    bool                 comInitialized_ = false;  // tracks our CoInitializeEx so shutdown() is idempotent

    uint32_t deviceSampleRate_ = 0;
    uint32_t deviceChannels_   = 0;

    // Mic packets are variable-size; we emit fixed 480-sample frames, so we accumulate across packets.
    AudioFrame accumulator_{};
    size_t     accumulatorPos_ = 0;
};
