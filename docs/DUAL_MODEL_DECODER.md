# Dual-model EnCodec work

This branch started with decoding because the Android application does not need an
encoder. Decoder-only model files remain supported, so the Android APK does not
need to ship encoder weights. A combined model is available for desktop or
server-side 48 kHz encoding.

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
- A non-LM ECDC decompressor with per-frame scales, exact code lengths, Meta-style
  overlap-add, Float32 WAV output and optional peak rescaling.
- A version-2 combined model containing encoder, decoder and shared RVQ weights.
- A configurable 24/48 kHz encoder graph with stereo input, non-causal padding,
  GroupNorm and Meta-compatible frame normalization for the 48 kHz model.
- A non-LM WAV-to-ECDC compressor with 48 kHz segmentation and selectable bitrate.
- Correct ceil-to-hop padding for partial final encoder frames.

The generated 48 kHz float32 files are approximately 36 MiB for decoder-only and
65 MiB for the combined encoder/decoder model. They are build artifacts and are
intentionally not committed.

## Validation status

Both model variants load and produce the expected number of samples. The legacy
24 kHz tests still pass. A 222.16-second official 48 kHz stereo ECDC file decoded
to exactly 10,663,680 frames. Comparison with Meta's Python CLI output produced
correlation rounding to 1.0 and approximately 1.8e-5 RMS difference after matching
gain; that residual is consistent with the reference WAV's Int16 quantization.

The 48 kHz macOS smoke test decoded 222.16 seconds in 34.53 seconds using the
single-threaded Eigen path. Independent frame workers produced byte-identical
output at every tested thread count. On the test Mac, two workers reduced this
to 20.30 seconds, three to 18.57 seconds, and four to 17.49 seconds. Two workers
are the default because additional workers sharply increase aggregate CPU time
for relatively small wall-time gains. Listening and Android device tests are
still required.

The 48 kHz encoder completed a native smoke test: a 52,800-frame Float32 stereo
WAV was encoded at 3 kbps into two non-LM ECDC segments and decoded back to
exactly 52,800 frames. This validates the combined model, tail-hop padding,
per-frame scales and ECDC framing; broader interoperability fixtures are still
desirable.

## Next milestones

1. Add permanent Python parity fixtures to automated tests.
2. Add Android NDK/arm64 build support and verify Eigen NEON vectorization.
3. Benchmark speed, memory and energy against the current ONNX Runtime decoder on
   the OnePlus 13 and a lower-range phone.
4. Add a decoder-only build option so the CLI/APK does not carry legacy compiled
   encoder and 24 kHz weights.
5. Consider float16 or quantized weight storage after float32 parity is established.
