# Dual-model decoder work

This branch starts with decoding because the Android application does not need an
encoder. Keeping the decoder and its weights separate also avoids shipping the
upstream encoder weights in the APK.

## Implemented in the first milestone

- A versioned decoder-only binary model format.
- A reproducible exporter for official Meta 24 kHz and 48 kHz checkpoints.
- Runtime model metadata: sample rate, channels, codebook count, causality and
  normalization.
- One Eigen decoder graph configurable for 24 kHz causal mono and 48 kHz
  non-causal stereo.
- 48 kHz time GroupNorm, symmetric reflection padding and symmetric transposed
  convolution trimming.
- RVQ tables loaded from the selected model, with 32 codebooks at 24 kHz and 16
  at 48 kHz.
- An exact code-frame decode overload so byte padding cannot create a false frame.
- A raw-packet diagnostic utility for comparison with Meta's Python decoder.

The generated float32 files are approximately 44 MiB for 24 kHz and 36 MiB for
48 kHz. They are build artifacts and are intentionally not committed.

## Validation status

Both model variants load and produce the expected number of samples. The legacy
24 kHz tests still pass. Initial parity tests show that the exported 24 kHz model
matches this repository's compiled decoder weights, but neither path is yet
sample-accurate with Meta's Python output. The new 48 kHz path has the correct
shape and strong output correlation, but also needs layer-by-layer parity work.
It must not be integrated into the Android player until that difference is
understood.

## Next milestones

1. Add layer-level reference vectors and correct numerical differences.
2. Parse non-LM `.ecdc` framing, per-frame scale values and 48 kHz overlap-add.
3. Add WAV output and optional decoder-side peak rescaling to the CLI.
4. Add Android NDK/arm64 build support and verify Eigen NEON vectorization.
5. Benchmark speed, memory and energy against the current ONNX Runtime decoder on
   the OnePlus 13 and a lower-range phone.
6. Consider float16 or quantized weight storage after float32 parity is established.
