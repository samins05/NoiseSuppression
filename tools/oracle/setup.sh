#!/bin/sh
# Reproducibly build the RNNoise "golden oracle" used to verify our suppressor.
#
# We pin the classic v0.1.1 release (22 Bark bands, 42 features, weights committed in
# src/rnn_data.c — the architecture our reimplementation targets). We then apply
# instrument.patch, which adds an RNNOISE_DUMP-gated dump of per-frame intermediates
# (Ex[22], features[42], raw RNN gains[22], final gains[22]) so each DSP slice can diff
# against this reference. The pristine denoise path is untouched unless RNNOISE_DUMP is set.
#
# Usage (from repo root):  sh tools/oracle/setup.sh
# Produces: third_party/rnnoise_classic/rnnoise_demo.exe
#
# third_party/ is gitignored; this script + instrument.patch are the tracked source of truth.
set -e

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DEST="$REPO_ROOT/third_party/rnnoise_classic"
GCC="${GCC:-/c/msys64/mingw64/bin/gcc.exe}"

if [ ! -d "$DEST" ]; then
  echo "Cloning RNNoise v0.1.1 into $DEST"
  git clone --branch v0.1.1 --depth 1 https://github.com/xiph/rnnoise "$DEST"
fi

cd "$DEST"
# Apply instrumentation once (idempotent: skip if already applied).
if ! grep -q "oracle instrumentation" src/denoise.c; then
  echo "Applying instrument.patch"
  git apply "$REPO_ROOT/tools/oracle/instrument.patch"
fi

echo "Building rnnoise_demo.exe with $GCC"
"$GCC" -O2 -Iinclude -Isrc \
  src/denoise.c src/kiss_fft.c src/pitch.c src/celt_lpc.c src/rnn.c src/rnn_data.c src/rnn_reader.c \
  examples/rnnoise_demo.c -lm -o rnnoise_demo.exe

echo "Done: $DEST/rnnoise_demo.exe"
echo "Run:  RNNOISE_DUMP=dump.txt $DEST/rnnoise_demo.exe noisy.pcm clean.pcm"
