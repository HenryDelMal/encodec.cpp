# Runtime model files

The fork supports two little-endian Float32 runtime formats. Both start with the
`ENCDMOD` magic and carry model metadata: sample rate, channel count, maximum
quantizers, causality and normalization.

## Version 1: decoder-only

Version 1 stores:

1. Decoder tensors.
2. RVQ codebooks.

Use it for playback applications, particularly Android, because it omits all
encoder tensors.

## Version 2: combined

Version 2 stores:

1. Encoder tensors.
2. Decoder tensors.
3. Shared RVQ codebooks.

`encodec_compress` requires this format. `encodec_decompress` can load either
version.

## Compiled weights versus runtime models

The three compressed C++ files in `src/lib/weights/` predate the runtime-model
format. CMake extracts them into generated C++ translation units and links them
into the library. They implement the legacy no-argument 24 kHz API.

Do not rename them to `.bin` or pass them to the exporter. They do not contain a
PyTorch checkpoint dictionary, and the exporter cannot reconstruct 48 kHz
weights from them.

`tools/export_decoder_model.py` instead reads the official Meta state dictionary,
reorders convolution tensors for the Eigen graph, resolves weight normalization,
and writes the required contiguous tensor sequence.

## Which model should I deploy?

| Use case | Recommended file |
|---|---|
| Android or desktop playback only | matching decoder-only version 1 |
| Native WAV-to-ECDC compression | matching combined version 2 |
| Both encoding and decoding in one server process | combined version 2 |
| Original embedded 24 kHz API | no runtime file required |

Do not load a 24 kHz ECDC file with a 48 kHz model, or the reverse. The CLI checks
the model name in ECDC metadata and rejects a mismatch.

## Reproducible generation

Use the exact official checkpoint names:

```text
encodec_24khz-d7cc33bc.th
encodec_48khz-7e698e3e.th
```

The hash suffix is part of Meta's checkpoint filename. Keep it in local download
scripts so an accidentally substituted model is easier to notice.

The exporter needs only PyTorch. It performs CPU tensor conversion and writes
Float32 data; no GPU is required.

## Troubleshooting

### `Cannot open EnCodec model`

The path passed to `--model` does not exist from the current working directory.
Generate the runtime file first, then verify it with `ls -lh models/`.

### `This model file does not contain encoder weights`

`encodec_compress` was given a decoder-only version-1 file. Regenerate the same
checkpoint with `--include-encoder`.

### `ECDC and decoder model do not match`

The file and runtime model use different sample rates/model variants.

### WAV format mismatch

The compressor does not resample automatically. Provide 24 kHz mono to the 24
kHz model and 48 kHz stereo to the 48 kHz model.

### Process is killed while encoding a long 24 kHz file

Use a build containing the bounded-memory encoder. It automatically selects a
30-second chunk for files longer than 60 seconds. Older revisions passed the
complete waveform through the network at once and could allocate many gigabytes
of intermediate tensors.

Every non-final 24 kHz chunk is aligned to the 320-sample codec hop. A one-second
history window warms the causal convolution and LSTM state before each chunk;
override it with `--warmup-seconds N`. The history codes are discarded and do
not alter duration or bitrate.

The decoder also selects bounded chunks automatically. Its choice accounts for
the requested frame-worker count: approximately `80 / threads` seconds, clamped
to 5–30 seconds. Both CLIs print the selected seconds and aligned sample counts.
Manual `--chunk-seconds` and `--warmup-seconds` values remain available.

### Large repository or executable size

The legacy weights under `src/lib/weights/` are linked into the library. A future
decoder-only build option can remove those arrays for APK-focused builds. Until
then, do not additionally package a combined runtime model when the application
only decodes.
