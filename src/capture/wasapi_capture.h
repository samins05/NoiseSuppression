#pragma once

#include "../core/ring_buffer.h"

#include <array>
#include <atomic>
#include <cstdint>

// Forward-declare COM interfaces so callers don't need <mmdeviceapi.h> / <windows.h>.
struct IMMDeviceEnumerator;
struct IMMDevice;
struct IAudioClient;
struct IAudioCaptureClient;

// 10 ms of mono audio at 48 kHz : the frame size RNNoise requires downstream.
using AudioFrame = std::array<float, 480>;

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
    void requestStop();  // Set the stop flag and wake the hot loop. Safe to call from any thread.

    uint64_t framesProduced() const noexcept {
        return framesProduced_.load(std::memory_order_relaxed);
    }
    uint64_t framesDropped() const noexcept {
        return framesDropped_.load(std::memory_order_relaxed);
    }

private:
    RingBuffer<AudioFrame>& output_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> framesProduced_{0};
    std::atomic<uint64_t> framesDropped_{0};

    // COM handles, owned by initialize() / shutdown(). Manual Release.
    IMMDeviceEnumerator* enumerator_    = nullptr;
    IMMDevice*           device_        = nullptr;
    IAudioClient*        audioClient_   = nullptr;
    IAudioCaptureClient* captureClient_ = nullptr;
    void*                eventHandle_   = nullptr; // HANDLE; void* keeps the header WinAPI-free.

    uint32_t deviceSampleRate_ = 0;
    uint32_t deviceChannels_   = 0;

    // Mic packets are variable-size; we emit fixed 480-sample frames, so we accumulate across packets.
    AudioFrame accumulator_{};
    size_t     accumulatorPos_ = 0;
};
