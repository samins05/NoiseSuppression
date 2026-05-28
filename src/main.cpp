#include "core/ring_buffer.h"
#include "capture/wasapi_capture.h"
#include "suppressor/suppressor.h"
#include "processing/processing_stage.h"

#include <chrono>
#include <iostream>
#include <thread>

int main() {

    /*
    Capture thread     -> produces 100 frames/s (raw audio)   -> RawAudioBuffer
    Processing thread  -> drains RawAudioBuffer, suppresses    -> CleanAudioBuffer
    main (for now)     -> drains CleanAudioBuffer, prints stats
    */
    constexpr size_t frameBufferSize = 128; // frames that each ring buffer can hold
    constexpr size_t LOOP_DURATION_SEC = 40;
    RingBuffer<AudioFrame> RawAudioBuffer(frameBufferSize);
    RingBuffer<AudioFrame> CleanAudioBuffer(frameBufferSize);

    std::cout << "RawAudioBuffer capacity = "   << RawAudioBuffer.capacity()   << " frames\n";
    std::cout << "CleanAudioBuffer capacity = " << CleanAudioBuffer.capacity() << " frames\n";

    WasapiCapture capture(RawAudioBuffer);
    try {
        capture.initialize();
    } catch (const std::exception& e) {
        std::cerr << "WasapiCapture::initialize failed: " << e.what() << "\n";
        return 1;
    }

    // Suppressor is the pure transform; ProcessingStage owns the loop that pumps
    // RawAudioBuffer -> suppressor -> CleanAudioBuffer. (Passthrough for now.)
    Suppressor suppressor;
    ProcessingStage processing(RawAudioBuffer, CleanAudioBuffer, suppressor);

    // main owns all pipeline thread lifetimes
    std::thread captureThread(&WasapiCapture::run, &capture);
    std::thread processingThread(&ProcessingStage::run, &processing);

    using namespace std::chrono;
    AudioFrame drained;
    uint64_t cleanDrained = 0;

    // Drain CleanAudioBuffer every second and print stats. main is the sole consumer of
    // CleanAudioBuffer; the processing thread is the sole consumer of RawAudioBuffer.
    // (Once Phase 3 lands, the playback thread takes over draining CleanAudioBuffer.)
    for (size_t sec = 1; sec <= LOOP_DURATION_SEC; ++sec) {
        std::this_thread::sleep_for(seconds(1));
        size_t thisTick = 0;
        while (CleanAudioBuffer.read(drained)) {
            ++thisTick;
            ++cleanDrained;
        }
        std::cout << "[t=" << sec << "s]"
                  << " captured="   << capture.framesProduced()
                  << " processed="  << processing.framesProcessed()
                  << " cleanDrained=" << cleanDrained
                  << " this_tick="  << thisTick
                  << " procDropped=" << processing.framesDropped()
                  << std::endl;
    }

    // Stop the producer of RawAudioBuffer first so processing can drain the tail, then stop processing.
    capture.requestStop();
    captureThread.join();
    processing.requestStop();
    processingThread.join();
    capture.shutdown(); // idempotent; also runs from the destructor as main returns.
    return 0;
}
