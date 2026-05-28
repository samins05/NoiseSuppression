# Noise Suppression Tool — Claude Code Context

## What We're Building
A real-time, device-level noise suppression pipeline in C++. It sits between a physical microphone and any application (Discord, CS2, Zoom) by routing cleaned audio through a virtual device. The user never configures noise suppression per-app — it's handled once at the OS level.

**The problem it solves:** CS2/Discord's built-in noise suppression has a fixed threshold that's too aggressive for quiet speakers and can't be tuned per microphone. This tool bypasses that entirely by cleaning audio before it reaches any app.

---

## Tech Stack
- **Language:** C++ (C++17)
- **Audio capture:** WASAPI (Windows Audio Session API)
- **Noise suppression:** RNNoise (Mozilla) — we implement the inference pipeline ourselves using their pre-trained weights
- **Virtual device:** VB-Cable (free virtual audio driver)
- **Synchronization:** Lock-free ring buffers using std::atomic

---

## Build & Run

- **Toolchain:** MSYS2 MinGW-w64 GCC at `C:\msys64\mingw64\bin\g++.exe`. The compiler must use the **posix** thread model — `std::thread` is unsupported on win32-model GCC builds.
- **Build:** `.\build.ps1` from the project root. The script pins the compiler by absolute path, prepends its bin directory to PATH so g++'s subprocesses (cc1plus, as, ld) resolve to the same toolchain, and links the C++ runtime statically (`-static -static-libgcc -static-libstdc++`) so the output `.exe` has no DLL dependencies.
- **Run main:** `.\build\main.exe`.
- **Run tests:**
  - `.\build\test_ring_buffer.exe` — expected: `Ring buffer test passed: 100000 ordered items.`
  - `.\build\test_wasapi_capture.exe` — expected: `WasapiCapture test passed: 7 cases (framing, mixing, silent, overflow).`
  - `.\build\test_processing_stage.exe` — expected: `ProcessingStage test passed: 4 cases (flow, identity, order, overflow).`
- Adding more tests means adding a corresponding `g++` invocation to [build.ps1](build.ps1) — there is no test discovery.

---

## System Design

### Functional Requirements
- Capture live mic audio
- Suppress background noise in real time
- Output cleaned audio to a virtual device any app can use

### Non-Functional Requirements
- End-to-end latency < 20ms
- No audible glitches after hours of use
- No dynamic memory allocation after startup

### High Level Pipeline
```
Physical Mic
    → [Capture Thread] WASAPI capture
    → Ring Buffer 1 (raw PCM)
    → [Processing Thread] RNNoise suppressor
    → Ring Buffer 2 (clean PCM)
    → [Playback Thread] VB-Cable virtual device
    → Any app (Discord, CS2, Zoom)
```

---

## Core Components

### RingBuffer (src/core/ring_buffer.h)
- Fixed-size circular array, pre-allocated at startup
- Lock-free: two std::atomic pointers (write_ptr, read_ptr)
- write() called by producer thread, read() called by consumer thread
- Each pointer only written by one thread — no mutex needed
- Memory ordering: acquire/release on pointer increments
  - Write frame THEN increment write_ptr (release)
  - Read ptr with acquire THEN read frame
  - Prevents CPU reordering pointer increment before data is committed
- bool write(float* frame) — returns false if full
- bool read(float* frame) — returns false if empty
- Two instances: `RawAudioBuffer` (mic → suppressor), `CleanAudioBuffer` (suppressor → playback)

**Key Constraints:**
- Fixed size decided at startup, never changes
- Overflow: define behavior explicitly (drop oldest or drop newest)
- Underflow on CleanAudioBuffer is more dangerous — outputs directly to playback. Output silence or repeat last frame
- Size directly controls latency: each frame held = 10ms added

---

### WASAPI Capture (src/capture/wasapi_capture.h)
- Class: WasapiCapture — owns WASAPI handles and the hot-loop function. The `std::thread` that runs `run()` is owned by [src/main.cpp](src/main.cpp); the class is a passive component.
- Public lifecycle: `initialize()` → caller spawns `std::thread(&WasapiCapture::run, &capture)` → `requestStop()` + caller `join()`s → `shutdown()` (also runs from destructor). Destroying the object while a thread is still inside `run()` is asserted against.
- Initialization chain: IMMDeviceEnumerator → IMMDevice → IAudioClient → IAudioCaptureClient
- IAudioCaptureClient is the only interface the capture thread holds after init
- Event-driven hot loop: thread sleeps via WaitForSingleObject until Windows signals frames are ready
- On wakeup: GetBuffer() → copy to RawAudioBuffer → ReleaseBuffer() immediately
- You do NOT own the Windows buffer — copy and release as fast as possible
- Mode: shared (MVP), exclusive (post-MVP) — configurable parameter at init
- MMCSS enrollment at thread startup (post-MVP but document where it goes)

**Key Constraints:**
- Sample rate fixed at 48kHz, frame size fixed at 480 samples — required by RNNoise downstream
- Buffer size requested from WASAPI is not guaranteed — call GetBufferSize() after init
- Handle AUDCLNT_E_DEVICE_INVALIDATED in hot loop — mic can be unplugged at runtime
- Exclusive mode requires device not already in use — fallback to shared mode on failure

---

### RNNoise Suppressor (src/suppressor/)
- Pure processing component — not a thread, no WASAPI, no ring buffers
- Interface: takes raw PCM frame in, returns clean PCM frame out
- Uses Mozilla's pre-trained weights (weights.h — just a float array, no file loading)

**Pipeline (must complete in < 10ms):**
1. Receive raw frame from RawAudioBuffer
2. FFT — convert 480 time-domain samples to frequency spectrum (identify voice vs noise bands)
3. Bark Scale Conversion — compress FFT bins into 22 bands matching human hearing
4. Feature Extraction — compute energy per band + frame-to-frame energy changes
5. GRU Forward Pass — feed features into network, outputs 22 gain values (0.0–1.0 per band)
6. Apply Gains — multiply each frequency band by gain (noise attenuated, voice preserved)
7. Inverse FFT — convert back to 480 time-domain samples
8. Overlap-Add — blend edges of adjacent frames to eliminate click artifacts
9. Write clean frame to CleanAudioBuffer

**Key Constraints:**
- GRU is stateful — hidden state persists between frames, never reset mid-session
- Overlap-Add requires previous frame tail in memory — pre-allocate one extra frame
- FFT size fixed by frame size — 480 samples in, fixed FFT size out
- Pipeline must complete in < 10ms — next frame arrives regardless

**File breakdown:**
- suppressor.cpp — wires full pipeline
- fft.h/cpp — FFT + IFFT (implement Cooley-Tukey or use FFTW)
- bark.h/cpp — Bark scale conversion + feature extraction
- gru.h/cpp — GRU forward pass (matrix multiply + tanh/sigmoid activations)
- weights.h — Mozilla's pre-trained weights as float array

---

### VB-Cable Playback (src/playback/wasapi_playback.h)
- Class: WasapiPlayback — mirror of WasapiCapture, opposite direction
- Reads clean frames from CleanAudioBuffer at a fixed steady rate
- Writes to VB-Cable virtual device which apps see as a normal mic
- Playback thread runs on strict clock — cannot wait, cannot skip

---

## Threading Model
Three concurrent threads, communicate only through ring buffers:
- **Capture thread** — feeds RawAudioBuffer
- **Processing thread** — drains RawAudioBuffer, runs suppressor, feeds CleanAudioBuffer
- **Playback thread** — drains CleanAudioBuffer, writes to virtual device

The two ring buffer instances are named **RawAudioBuffer** (mic → suppressor) and **CleanAudioBuffer** (suppressor → playback). Never abbreviate to `rb1` / `rb2`.

---

## File Structure (build incrementally, don't create upfront)
```
noise-suppressor/
├── src/
│   ├── main.cpp                  # owns thread lifetime, creates RawAudioBuffer + CleanAudioBuffer
│   ├── core/
│   │   ├── audio_frame.h         # AudioFrame alias, shared by capture/suppressor/processing
│   │   ├── ring_buffer.h
│   │   └── ring_buffer.inl       # template defs, #included by ring_buffer.h (not compiled standalone)
│   ├── capture/
│   │   ├── wasapi_capture.h
│   │   └── wasapi_capture.cpp
│   ├── processing/
│   │   ├── processing_stage.h    # owns the RawAudioBuffer → suppressor → CleanAudioBuffer loop
│   │   └── processing_stage.cpp
│   ├── playback/
│   │   ├── wasapi_playback.h
│   │   └── wasapi_playback.cpp
│   └── suppressor/
│       ├── suppressor.h
│       ├── suppressor.cpp
│       ├── fft.h / fft.cpp
│       ├── bark.h / bark.cpp
│       ├── gru.h / gru.cpp
│       └── weights.h
├── tests/
│   ├── test_ring_buffer.cpp
│   ├── test_fft.cpp
│   ├── test_suppressor.cpp
│   └── test_passthrough.cpp
└── docs/
    └── system_design.md
```

---

## Implementation Order (strict — do not skip phases)

### Phase 1 — RingBuffer ✅ DONE
Files: [src/core/ring_buffer.h](src/core/ring_buffer.h), [src/core/ring_buffer.cpp](src/core/ring_buffer.cpp), [tests/test_ring_buffer.cpp](tests/test_ring_buffer.cpp)
- `RingBuffer<T>` is a **single-producer, single-consumer** template. Each ring buffer instance is owned by exactly two threads — one writer, one reader. The three-thread pipeline uses two SPSC instances (capture→processing and processing→playback), never one MPMC buffer.
- **Storage layout:** allocates `capacity + 1` slots and reports `capacity()` as `capacity_ - 1`. One slot is always left empty so `next(write_ptr) == read_ptr` unambiguously means "full" and `write_ptr == read_ptr` means "empty" — no separate count variable, which would itself require synchronization.
- **Memory ordering:** writer release-stores `write_ptr_` after writing the slot; reader acquire-loads `write_ptr_` before reading. Symmetric for `read_ptr_`. Each thread uses `memory_order_relaxed` when loading its own pointer (no synchronization needed with itself).
- **Template pattern:** the header `#include`s `ring_buffer.cpp` at the bottom so all template definitions live in one logical unit. Do **not** add `ring_buffer.cpp` to the compile list in [build.ps1](build.ps1) — it would compile as an empty translation unit.
- Test exercises 100k items through a 128-capacity buffer, asserts strict ordering. Run via `.\build\test_ring_buffer.exe`.

### Phase 2 — WASAPI Capture ✅ DONE
Files: [src/capture/wasapi_capture.h](src/capture/wasapi_capture.h), [src/capture/wasapi_capture.cpp](src/capture/wasapi_capture.cpp)
- `WasapiCapture` is a passive component — it takes a `RingBuffer<AudioFrame>&` (the `RawAudioBuffer` allocated in `main()`) and exposes `initialize()` / `run()` / `requestStop()` / `shutdown()`. `main` owns the `std::thread` that runs `run()`; `requestStop()` flips an atomic and `SetEvent`s the WASAPI wake handle so the hot loop returns promptly.
- The hot loop is the canonical WASAPI shared-mode event-driven pattern: `WaitForSingleObject` on the audio event → `GetBuffer` → mix down to mono → push into a 480-sample accumulator → `RingBuffer::write()` on every full frame → `ReleaseBuffer` ASAP. The Windows buffer is never held across the frame-emit logic.
- COM interfaces (`IMMDeviceEnumerator`, `IMMDevice`, `IAudioClient`, `IAudioCaptureClient`) are **forward-declared in the header** and only `#include`d in the `.cpp`, so consumers of the header don't pull in `<windows.h>`.
- The sample→frame logic (mix multichannel down to mono, accumulate into 480-sample frames, write to the ring buffer) lives in `ingestSamples()`, split out of `run()` so it's unit-testable without hardware. `run()` is just the WASAPI event wrapper that hands packets to `ingestSamples`. Tests in [tests/test_wasapi_capture.cpp](tests/test_wasapi_capture.cpp) drive `ingestSamples` directly with synthetic buffers.
- Format is locked to the device's mix format (IEEE float; init throws if not). Sample rate is logged but not converted — a non-48 kHz device just produces a warning since the downstream RNNoise needs 48 kHz.
- `AUDCLNT_BUFFERFLAGS_SILENT` is honored (writes zeros instead of the buffer contents). Hot-swap recovery on `AUDCLNT_E_DEVICE_INVALIDATED` is a TODO — currently just logs and breaks.
- Build linkage: [build.ps1](build.ps1) adds `-lole32 -lwinmm -lksuser`. `ksuser` is needed for the `KSDATAFORMAT_SUBTYPE_IEEE_FLOAT` GUID that `INITGUID` doesn't emit (mmreg/ksmedia use a different macro).
- Verification: `.\build\main.exe` runs `WasapiCapture` for 10 s, draining frames from `RawAudioBuffer` in main and logging produced/consumed/dropped each second. Expected steady-state: `produced` climbs by ~100/sec (48000 / 480 = 100 frames per second), `dropped` stays at 0.

> **Phase order note:** Phase 4 (suppressor) is implemented **before** Phase 3 (playback). The playback thread drains `CleanAudioBuffer`, which is empty until the processing stage writes to it — so the suppressor/processing wiring has to exist first, otherwise playback has nothing to test against.

### Phase 4 — Suppressor + Processing Stage 🟡 passthrough slice DONE, DSP pending
Files: [src/suppressor/suppressor.h](src/suppressor/suppressor.h), [src/suppressor/suppressor.cpp](src/suppressor/suppressor.cpp), [src/processing/processing_stage.h](src/processing/processing_stage.h), [src/processing/processing_stage.cpp](src/processing/processing_stage.cpp)
- Split into two classes: **`Suppressor`** is the pure transform (`AudioFrame process(const AudioFrame&)`, no threads/buffers); **`ProcessingStage`** owns the read→process→write loop and holds `RawAudioBuffer&` + `CleanAudioBuffer&` + `Suppressor&`. `ProcessingStage` is passive like `WasapiCapture` (`run()` / `requestStop()`); `main` owns the processing `std::thread`.
- First slice is **passthrough** — `Suppressor::process` returns its input unchanged. This verified the full capture→processing→clean wiring before any DSP exists. `process()` is a non-const member because the real pipeline will carry GRU hidden state + overlap-add tail between frames.
- `ProcessingStage::run` yields when `RawAudioBuffer` is empty and counts drops if `CleanAudioBuffer` is full; no event handle needed since it's purely buffer-driven. The loop body is factored into `pumpOnce()` (read → suppress → write, one frame) so the flow is unit-testable without a thread — `run()` just calls it in a loop. Tests in [tests/test_processing_stage.cpp](tests/test_processing_stage.cpp).
- Remaining DSP slice (still TODO): FFT (test in isolation against known values) → Bark conversion + feature extraction → GRU forward pass using Mozilla's weights → gain application + IFFT + overlap-add. Slots into `Suppressor::process` with zero changes to threading or buffers.
- Verification: `.\build\main.exe` runs capture + processing threads; `captured` and `processed` both climb ~100/sec, `procDropped=0`.

### Phase 3 — Passthrough Pipeline (playback)
Files: wasapi_playback.h, wasapi_playback.cpp
- Get playback thread reading CleanAudioBuffer and writing to VB-Cable (replaces main's temporary drain loop as the consumer of CleanAudioBuffer)
- Full passthrough: mic in → same audio out, no cleaning
- Open Discord, select virtual mic, verify voice comes through
- This is your first real end-to-end test

### Phase 4 (DSP slice) — RNNoise internals
Files: suppressor/, weights.h
- Implement FFT first, test in isolation against known values
- Add Bark conversion + feature extraction
- Add GRU forward pass using Mozilla's weights
- Add gain application + IFFT + overlap-add
- Plug into processing thread between RawAudioBuffer and CleanAudioBuffer
- Verify before/after by writing WAV files and listening

---

## Post-MVP Optimizations (document now, implement after MVP works)
- WASAPI exclusive mode — bypasses Windows audio mixer, lowest latency
- MMCSS thread enrollment — OS scheduling priority for audio threads
- CPU core pinning via SetThreadAffinityMask
- No syscalls in hot loop
- Profile and tune buffer sizes for minimum latency without glitches

---

## How To Verify It Works
1. Write before/after WAV files — listen directly, completely objective
2. Passthrough test — open Discord with a friend, switch to virtual mic, talk normally
3. Stress test — run for a full CS2 session, verify no drift, glitches, or crashes
4. Latency measurement — measure end-to-end with timestamps at capture and playback

---

## Key Things To Know Going In
- You do not own the WASAPI buffer — copy immediately and release
- Write pointer increment is the signal to the consumer thread — data must be fully written before pointer increments (acquire/release memory ordering)
- CleanAudioBuffer underflow is more dangerous than RawAudioBuffer overflow — playback thread cannot wait
- GRU hidden state must persist between frames — never reset it mid-session
- Overlap-Add is not optional — without it you get 100 audible clicks per second
- Buffer size is a latency tradeoff — each frame in buffer = 10ms latency added