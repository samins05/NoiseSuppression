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
- **Run tests:** `.\build\test_ring_buffer.exe` (currently the only test). Expected output: `Ring buffer test passed: 100000 ordered items.`
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
- Two instances: rb1 (raw audio), rb2 (clean audio)

**Key Constraints:**
- Fixed size decided at startup, never changes
- Overflow: define behavior explicitly (drop oldest or drop newest)
- Underflow on RB2 is more dangerous — outputs directly to playback. Output silence or repeat last frame
- Size directly controls latency: each frame held = 10ms added

---

### WASAPI Capture (src/capture/wasapi_capture.h)
- Class: WasapiCapture — owns thread, WASAPI handles, lifetime
- Initialization chain: IMMDeviceEnumerator → IMMDevice → IAudioClient → IAudioCaptureClient
- IAudioCaptureClient is the only interface the capture thread holds after init
- Event-driven hot loop: thread sleeps via WaitForSingleObject until Windows signals frames are ready
- On wakeup: GetBuffer() → copy to RB1 → ReleaseBuffer() immediately
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
1. Receive raw frame from RB1
2. FFT — convert 480 time-domain samples to frequency spectrum (identify voice vs noise bands)
3. Bark Scale Conversion — compress FFT bins into 22 bands matching human hearing
4. Feature Extraction — compute energy per band + frame-to-frame energy changes
5. GRU Forward Pass — feed features into network, outputs 22 gain values (0.0–1.0 per band)
6. Apply Gains — multiply each frequency band by gain (noise attenuated, voice preserved)
7. Inverse FFT — convert back to 480 time-domain samples
8. Overlap-Add — blend edges of adjacent frames to eliminate click artifacts
9. Write clean frame to RB2

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
- Reads clean frames from RB2 at a fixed steady rate
- Writes to VB-Cable virtual device which apps see as a normal mic
- Playback thread runs on strict clock — cannot wait, cannot skip

---

## Threading Model
Three concurrent threads, communicate only through ring buffers:
- **Capture thread** — feeds RB1
- **Processing thread** — drains RB1, runs suppressor, feeds RB2
- **Playback thread** — drains RB2, writes to virtual device

---

## File Structure (build incrementally, don't create upfront)
```
noise-suppressor/
├── src/
│   ├── main.cpp                  # owns thread lifetime, creates rb1 + rb2
│   ├── core/
│   │   ├── ring_buffer.h
│   │   └── ring_buffer.cpp
│   ├── capture/
│   │   ├── wasapi_capture.h
│   │   └── wasapi_capture.cpp
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

### Phase 2 — WASAPI Capture
Files: wasapi_capture.h, wasapi_capture.cpp
- Get real mic audio flowing into RB1
- Processing thread just copies RB1 → RB2 directly, no suppression yet
- Verify frames arrive correctly and timing is stable

### Phase 3 — Passthrough Pipeline
Files: wasapi_playback.h, wasapi_playback.cpp
- Get playback thread reading RB2 and writing to VB-Cable
- Full passthrough: mic in → same audio out, no cleaning
- Open Discord, select virtual mic, verify voice comes through
- This is your first real end-to-end test

### Phase 4 — RNNoise Suppressor
Files: suppressor/, weights.h
- Implement FFT first, test in isolation against known values
- Add Bark conversion + feature extraction
- Add GRU forward pass using Mozilla's weights
- Add gain application + IFFT + overlap-add
- Plug into processing thread between RB1 and RB2
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
- RB2 underflow is more dangerous than RB1 overflow — playback thread cannot wait
- GRU hidden state must persist between frames — never reset it mid-session
- Overlap-Add is not optional — without it you get 100 audible clicks per second
- Buffer size is a latency tradeoff — each frame in buffer = 10ms latency added