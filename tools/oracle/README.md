# RNNoise golden oracle

The suppressor's DSP is verified by diffing against a reference RNNoise build, not by
eyeballing output. This directory reproducibly builds that reference.

## What it is
- **Pinned version:** Xiph RNNoise **v0.1.1** — the "classic" architecture our reimplementation
  targets: `FRAME_SIZE 480`, `WINDOW_SIZE 960`, **22 Bark bands**, **42 features**
  (`NB_BANDS + 3*NB_DELTA_CEPS + 2`), pretrained weights committed in `src/rnn_data.c`.
  (The current RNNoise `master` was rewritten to 32 bands / 65 features with weights that
  download from a server — deliberately *not* what we use.)
- **instrument.patch** adds a dump hook to `rnnoise_process_frame`. It is a no-op unless the
  `RNNOISE_DUMP` environment variable names an output file.

## Build it
```sh
sh tools/oracle/setup.sh
```
Clones into `third_party/rnnoise_classic/` (gitignored), applies the patch, builds
`rnnoise_demo.exe`. Requires the MSYS2 MinGW gcc (override with `GCC=... sh tools/oracle/setup.sh`).

## Use it
The demo speaks **raw 16-bit mono 48 kHz PCM** (not WAV — bridge via `wav_io::writeRawPcm16`
/ `readRawPcm16` in `tests/wav_io.h`). It also **drops the first output frame** (warmup).

```sh
RNNOISE_DUMP=dump.txt third_party/rnnoise_classic/rnnoise_demo.exe noisy.pcm clean.pcm
```

`dump.txt` holds one block per frame:
```
FRAME silence=<0|1> vad=<float>
Ex   <22 band energies>
FEAT <42 features fed to the RNN>
GRNN <22 raw gains straight out of compute_rnn>   # Slice 6 (GRU) target
GFIN <22 final gains after pitch_filter+smoothing># Slice 7 (applied gains) target
```

## Which slice diffs against what
- Slice 3 (bands) → `Ex`
- Slice 4/5 (features) → `FEAT`
- Slice 6 (GRU) → `GRNN`
- Slice 7 (integration) → `GFIN` + the `clean.pcm` output samples
