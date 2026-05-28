#include "core/ring_buffer.h"
#include "capture/wasapi_capture.h"

#include <chrono>
#include <iostream>
#include <thread>

int main() {

    /*
    Capture thread -> produces 100 frames/s (raw audio) -> RawAudioBuffer
    */
    constexpr size_t frameBufferSize = 128; // frames that each ring buffer can hold 
    constexpr size_t LOOP_DURATION_SEC = 40;
    RingBuffer<AudioFrame> RawAudioBuffer(frameBufferSize);
    RingBuffer<AudioFrame> CleanAudioBuffer(frameBufferSize); // reserved for Phase 3

    std::cout << "RawAudioBuffer capacity = "   << RawAudioBuffer.capacity()   << " frames\n";
    std::cout << "CleanAudioBuffer capacity = " << CleanAudioBuffer.capacity() << " frames\n";

    WasapiCapture capture(RawAudioBuffer);
    try {
        capture.initialize();
    } catch (const std::exception& e) {
        std::cerr << "WasapiCapture::initialize failed: " << e.what() << "\n";
        return 1;
    }

    // main owns all pipeline thread lifetimes. Phase 3 will add processing + playback threads
    // alongside this one — keeping them in one place makes the concurrency model legible.
    std::thread captureThread(&WasapiCapture::run, &capture);

    using namespace std::chrono;
    AudioFrame drained;
    uint64_t consumed = 0;

    // drains the rawaudiobuffer every second and prints stats
    // **draining the rawaudiobuffer will be a task only done by processing thread 
    for (int sec = 1; sec <= LOOP_DURATION_SEC; ++sec) {
        std::this_thread::sleep_for(seconds(1));
        size_t thisTick = 0;
        while (RawAudioBuffer.read(drained)) {
            ++thisTick;
            ++consumed;
        }
        std::cout << "[t=" << sec << "s]"
                  << " produced="  << capture.framesProduced()
                  << " consumed="  << consumed
                  << " this_tick=" << thisTick
                  << " dropped="   << capture.framesDropped()
                  << std::endl;
    }

    capture.requestStop();
    captureThread.join();
    capture.shutdown(); //runs from its destructor as main returns.
    return 0;
}
