#define WIN32_LEAN_AND_MEAN
#define INITGUID  // emit GUID constants (CLSID_*, IID_*, KSDATAFORMAT_*) inline so we don't need libuuid

#include "wasapi_capture.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <combaseapi.h>

#include <cassert>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

// 480 samples = 10 ms at 48 kHz, which is what RNNoise expects
constexpr uint32_t        kTargetSampleRate = 48000; // kFrameSamples is how many samples we emit in each AudioFrame
constexpr uint32_t        kFrameSamples     = 480;     // Accumulate mic packets into fixed-size frames of 480 samples
constexpr REFERENCE_TIME  kBufferDur        = 200000;  // kBufferDur is the requested buffer duration for the WASAPI shared buffer

void throwIfFailed(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        // std::to_string would print decimal under an "0x" prefix — format as actual hex so the
        // HRESULT can be looked up directly (e.g. 0x80070006 = ERROR_INVALID_HANDLE).
        std::ostringstream oss;
        oss << what << " failed, HRESULT=0x" << std::hex << std::uppercase
            << static_cast<unsigned long>(hr);
        throw std::runtime_error(oss.str());
    }
}

} // namespace

WasapiCapture::WasapiCapture(RingBuffer<AudioFrame>& output) : output_(output) {}

WasapiCapture::~WasapiCapture() {
    // Contract: caller must have already requestStop()'d and joined the thread running run().
    // If a thread is still inside run() we'd be tearing down WASAPI underneath it.
    assert(!threadActive_.load(std::memory_order_acquire) && "destroyed while run() thread still active");
    shutdown();
}

void WasapiCapture::initialize() {
    // WASAPI is a COM API, so we must initialize COM before using it. We use the multithreaded
    // apartment model, which allows any thread to call any COM method without marshaling
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr != S_OK && hr != S_FALSE) {
        throwIfFailed(hr, "CoInitializeEx");
    }
    comInitialized_ = true; // so shutdown() balances exactly one CoUninitialize, even if called twice

    // Find the default capture mic
    throwIfFailed(CoCreateInstance(
                      CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                      IID_IMMDeviceEnumerator, reinterpret_cast<void**>(&enumerator_)),
                  "CoCreateInstance MMDeviceEnumerator");
    throwIfFailed(enumerator_->GetDefaultAudioEndpoint(eCapture, eCommunications, &device_),
                  "GetDefaultAudioEndpoint");

    // Open an audio client session on that device. This gives us access to the shared buffer and event-driven API.
    throwIfFailed(device_->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(&audioClient_)),
                  "Activate IAudioClient");

    // Query the device's native mix format 
    WAVEFORMATEX* mixFormat = nullptr;
    throwIfFailed(audioClient_->GetMixFormat(&mixFormat), "GetMixFormat");
    deviceSampleRate_ = mixFormat->nSamplesPerSec;
    deviceChannels_   = mixFormat->nChannels;

    // Validate the format is IEEE float: The downstream RNNoise expects 32-bit float samples. 
    bool isFloat = (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mixFormat);
        isFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    if (!isFloat) {
        CoTaskMemFree(mixFormat); // explicit cleanup before throwing
        throw std::runtime_error("Device mix format is not IEEE float; Phase 2 doesn't support conversion");
    }
    if (deviceSampleRate_ != kTargetSampleRate) {
        std::cerr << "WARNING: device sample rate is " << deviceSampleRate_
                  << " Hz, expected " << kTargetSampleRate
                  << " Hz (downstream suppressor will be wrong)\n";
    }
    std::cout << "Capture device: " << deviceSampleRate_ << " Hz, "
              << deviceChannels_ << " channel(s), IEEE float\n";

    // Initialize the audio client in event-driven shared mode
    // Mic can co-exist on multiple apps, window fixes a buffer size that can be used for all apps
    HRESULT initHr = audioClient_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        kBufferDur, 0, mixFormat, nullptr);
    CoTaskMemFree(mixFormat); // Initialize copied what it needed; mixFormat done with.
    throwIfFailed(initHr, "AudioClient Initialize");

    // The size we requested is not binding — the engine allocates at least that much and rounds to
    // whole frames, so read back what we actually got. It bounds how much audio can queue up before
    // we must drain, i.e. our latency budget.
    UINT32 bufferFrames = 0;
    if (SUCCEEDED(audioClient_->GetBufferSize(&bufferFrames))) {
        std::cout << "WASAPI buffer: " << bufferFrames << " frames ("
                  << (deviceSampleRate_ ? (bufferFrames * 1000.0 / deviceSampleRate_) : 0.0)
                  << " ms)\n";
    }

    // Wire up the event handle + grab the capture client
    // CreateEventW makes the kernel event the audio engine will signal
    eventHandle_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle_) {
        throw std::runtime_error("CreateEventW failed");
    }
    throwIfFailed(audioClient_->SetEventHandle(static_cast<HANDLE>(eventHandle_)), "SetEventHandle");
    throwIfFailed(audioClient_->GetService(IID_IAudioCaptureClient,
                                           reinterpret_cast<void**>(&captureClient_)),
                  "GetService IAudioCaptureClient");
}

void WasapiCapture::run() {
    // Flag the thread as inside run() for the whole scope, including the early-return paths below,
    // so the destructor's assert can actually tell whether a thread is still live in here.
    struct ActiveGuard {
        std::atomic<bool>& flag;
        explicit ActiveGuard(std::atomic<bool>& f) : flag(f) {
            flag.store(true, std::memory_order_release);
        }
        ~ActiveGuard() { flag.store(false, std::memory_order_release); }
    } activeGuard(threadActive_);

    // Each COM-using thread must CoInitialize.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) {
        std::cerr << "AudioClient Start failed, HRESULT=0x" << std::hex << hr << std::dec << "\n";
        CoUninitialize();
        return;
    }

    // Note: run() never writes the stop flag — it only reads it. A requestStop() that lands before
    // this point is therefore honored (the loop exits immediately) rather than being overwritten.
    while (!stopRequested_.load(std::memory_order_acquire)) {
        DWORD wait = WaitForSingleObject(static_cast<HANDLE>(eventHandle_), 200);
        if (wait != WAIT_OBJECT_0) {
            continue; // timeout or wake from requestStop() — re-check the stop flag
        }

        UINT32 packetSize = 0;
        captureClient_->GetNextPacketSize(&packetSize);
        while (packetSize > 0) {
            BYTE*  data      = nullptr;
            UINT32 numFrames = 0;
            DWORD  flags     = 0;
            HRESULT ghr = captureClient_->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
            if (ghr == AUDCLNT_S_BUFFER_EMPTY) {
                break;
            }
            if (FAILED(ghr)) {
                // TODO post-MVP: AUDCLNT_E_DEVICE_INVALIDATED → reinitialize on default endpoint change.
                std::cerr << "GetBuffer failed, HRESULT=0x" << std::hex << ghr << std::dec << "\n";
                break;
            }

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            ingestSamples(reinterpret_cast<const float*>(data), numFrames, deviceChannels_, silent);

            captureClient_->ReleaseBuffer(numFrames); // release ASAP — we do not own this buffer
            captureClient_->GetNextPacketSize(&packetSize);
        }
    }

    audioClient_->Stop();
    CoUninitialize();
}

void WasapiCapture::ingestSamples(const float* samples, uint32_t numFrames, uint32_t channels, bool silent) {
    for (uint32_t i = 0; i < numFrames; ++i) {
        float mono = 0.0f;
        // average channels (if multi-channel) into mono audio: (L+R)/2
        if (!silent) {
            for (uint32_t c = 0; c < channels; ++c) {
                mono += samples[i * channels + c];
            }
            mono /= static_cast<float>(channels);
        }
        accumulator_[accumulatorPos_++] = mono;
        if (accumulatorPos_ == kFrameSamples) {
            if (output_.write(accumulator_)) {
                framesProduced_.fetch_add(1, std::memory_order_relaxed);
            } else {
                framesDropped_.fetch_add(1, std::memory_order_relaxed);
            }
            accumulatorPos_ = 0;
        }
    }
}

void WasapiCapture::requestStop() {
    stopRequested_.store(true, std::memory_order_release);
    if (eventHandle_) {
        SetEvent(static_cast<HANDLE>(eventHandle_)); // wake the hot loop so it observes the flag
    }
}

void WasapiCapture::shutdown() {
    if (captureClient_) { captureClient_->Release(); captureClient_ = nullptr; }
    if (audioClient_)   { audioClient_->Release();   audioClient_   = nullptr; }
    if (device_)        { device_->Release();        device_        = nullptr; }
    if (enumerator_)    { enumerator_->Release();    enumerator_    = nullptr; }
    if (eventHandle_)   { CloseHandle(static_cast<HANDLE>(eventHandle_)); eventHandle_ = nullptr; }
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
}
