[![Ubuntu](https://github.com/HenryDelMal/encodec.cpp/actions/workflows/ubuntu.yml/badge.svg)](https://github.com/HenryDelMal/encodec.cpp/actions/workflows/ubuntu.yml)
[![macOS](https://github.com/HenryDelMal/encodec.cpp/actions/workflows/macos.yml/badge.svg)](https://github.com/HenryDelMal/encodec.cpp/actions/workflows/macos.yml)
[![Windows](https://github.com/HenryDelMal/encodec.cpp/actions/workflows/windows.yml/badge.svg)](https://github.com/HenryDelMal/encodec.cpp/actions/workflows/windows.yml)

# encodec.cpp

> **AI/vibecoding disclaimer:** This fork was modified substantially using
> vibecoded, AI-assisted development. Although the implemented paths have been
> compiled and smoke-tested, the changes have not received a full independent
> expert audit. Review and test the code carefully before relying on it in
> production, safety-critical, or resource-constrained deployments.

An experimental C++ implementation of Meta's [EnCodec](https://github.com/facebookresearch/encodec), based on
[pfeatherstone/encodec.cpp](https://github.com/pfeatherstone/encodec.cpp) and
[Eigen](https://gitlab.com/libeigen/eigen).

This fork adds runtime-selectable 24 kHz mono and 48 kHz stereo models, non-LM
`.ecdc` command-line tools, native 48 kHz encoding, parallel frame decoding,
OpenMP encoder threading and ARM NEON-friendly Release builds. Its primary goal
is a lighter native decoder for the
[Android EnCodec Player](https://github.com/HenryDelMal/Android-encodec-player).

This is experimental software. It does not support LM entropy-coded `.ecdc`
files, and Android performance and power consumption still require device-level
testing.

## Supported configurations

| Model | Audio | Codebooks | Runtime decode | Runtime encode |
|---|---:|---:|:---:|:---:|
| EnCodec 24 kHz | mono, causal | up to 32 | yes | yes |
| EnCodec 48 kHz | stereo, non-causal | up to 16 | yes | yes |

Use 1.5, 3, 6, 12 or 24 kbps with the 24 kHz model. The official 48 kHz model
uses 3, 6, 12 or 24 kbps.

## Build

Requirements:

- CMake 3.23 or newer
- A C++20 compiler
- Internet access during the first configuration so CPM can obtain Eigen
- Optional OpenMP runtime for `encodec_compress --threads N`

### macOS

```sh
brew install cmake ninja libomp

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DOpenMP_ROOT="$(brew --prefix libomp)"
cmake --build build --parallel
```

### Linux

With GCC:

```sh
sudo apt install build-essential cmake ninja-build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

With Clang, install the distribution's OpenMP package as well, commonly
`libomp-dev`.

Release builds enable LTO/IPO when the compiler supports it. On ARM64, Eigen
uses NEON without an additional runtime dependency. OpenMP is optional; the
library safely defaults to one intra-model thread even when OpenMP is present.

The relevant executables are:

```text
build/encodec_compress
build/encodec_decompress
build/encodec_decode_packet
build/encodec_tests
```

## Model weights: important distinction

This repository contains large files under `src/lib/weights/`:

```text
src/lib/weights/encoder.cpp.gz
src/lib/weights/decoder.cpp.gz
src/lib/weights/rvq.cpp.gz
```

They are C++ source arrays inherited from the upstream project and contain the
legacy 24 kHz model compiled into `libencodec`. They allow the original no-argument
`encodec::encoder` and `encodec::decoder` API to keep working.

They are **not** runtime model files, are **not** Meta `.th` checkpoints, and are
not inputs to `tools/export_decoder_model.py`. The command-line ECDC tools use
runtime `.bin` files generated from Meta's official checkpoints.

Runtime model files are deliberately not committed because they are large:

| Runtime file | Contents | Approximate size |
|---|---|---:|
| 24 kHz decoder-only | decoder + RVQ | 44 MiB |
| 24 kHz combined | encoder + decoder + RVQ | 73 MiB |
| 48 kHz decoder-only | decoder + RVQ | 36 MiB |
| 48 kHz combined | encoder + decoder + RVQ | 65 MiB |

Decoder-only files are suitable for the Android player. Combined files are
needed only by `encodec_compress` or another native encoder application.

## Generate runtime models

The exporter requires Python and PyTorch, but the resulting `.bin` files are
loaded by the C++ runtime without Python or an ML runtime.

Create an environment and download Meta's official checkpoints:

```sh
python3 -m venv .venv-model-export
source .venv-model-export/bin/activate
python -m pip install --upgrade pip torch

mkdir -p models/checkpoints
curl -L \
  -o models/checkpoints/encodec_24khz-d7cc33bc.th \
  https://dl.fbaipublicfiles.com/encodec/v0/encodec_24khz-d7cc33bc.th
curl -L \
  -o models/checkpoints/encodec_48khz-7e698e3e.th \
  https://dl.fbaipublicfiles.com/encodec/v0/encodec_48khz-7e698e3e.th
```

If Meta's Python EnCodec has already downloaded the models, they are normally in
the Torch Hub checkpoint cache, for example:

```text
~/.cache/torch/hub/checkpoints/encodec_24khz-d7cc33bc.th
~/.cache/torch/hub/checkpoints/encodec_48khz-7e698e3e.th
```

Generate decoder-only models:

```sh
python tools/export_decoder_model.py \
  --checkpoint models/checkpoints/encodec_24khz-d7cc33bc.th \
  --sample-rate 24000 \
  --output models/encodec-decoder-24khz-f32.bin

python tools/export_decoder_model.py \
  --checkpoint models/checkpoints/encodec_48khz-7e698e3e.th \
  --sample-rate 48000 \
  --output models/encodec-decoder-48khz-f32.bin
```

Generate combined encoder/decoder models by adding `--include-encoder`:

```sh
python tools/export_decoder_model.py \
  --checkpoint models/checkpoints/encodec_24khz-d7cc33bc.th \
  --sample-rate 24000 \
  --include-encoder \
  --output models/encodec-24khz-f32.bin

python tools/export_decoder_model.py \
  --checkpoint models/checkpoints/encodec_48khz-7e698e3e.th \
  --sample-rate 48000 \
  --include-encoder \
  --output models/encodec-48khz-f32.bin
```

See [docs/MODELS.md](docs/MODELS.md) for the file format, deployment choices and
model troubleshooting.

## Encode WAV to ECDC

The compressor accepts PCM16, PCM24, PCM32 and Float32 WAV input. Input must
already match the selected model:

- 24 kHz model: 24,000 Hz mono
- 48 kHz model: 48,000 Hz stereo

Convert with FFmpeg when necessary:

```sh
ffmpeg -i input.wav -ar 24000 -ac 1 input-24k-mono.wav
ffmpeg -i input.wav -ar 48000 -ac 2 input-48k-stereo.wav
```

Encode 24 kHz mono at 3 kbps using four intra-model threads:

```sh
build/encodec_compress \
  --model models/encodec-24khz-f32.bin \
  --input input-24k-mono.wav \
  --output output.ecdc \
  --bandwidth 3 \
  --threads 4
```

Encode 48 kHz stereo:

```sh
build/encodec_compress \
  --model models/encodec-48khz-f32.bin \
  --input input-48k-stereo.wav \
  --output output.ecdc \
  --bandwidth 12 \
  --threads 4
```

`--threads 1` is the memory- and power-efficient default. Threaded and
single-threaded encoding produce the same ECDC bytes.

## Decode ECDC to WAV

```sh
build/encodec_decompress \
  --model models/encodec-decoder-48khz-f32.bin \
  --input input.ecdc \
  --output output.wav \
  --threads 2 \
  --rescale
```

For decompression, `--threads` means independent ECDC frame workers, not OpenMP
intra-model threads. One worker uses the least memory. Multiple workers each own
a decoder and therefore duplicate its working tensors and weights.

The decoder preserves 48 kHz normalization scales and performs Meta-style
overlap-add. `--rescale` lowers the final peak to 0.99 when clipping would occur.

LM entropy-coded files are rejected. Generate files without Meta's `--lm` flag.

## C++ API

Legacy compiled-in 24 kHz API:

```cpp
encodec::encoder encoder;
encodec::decoder decoder;

std::vector<float> mono_audio(24000);
const unsigned int quantizers = encodec::get_encoded_nquantizers(3000);
const auto packet = encoder.encode(mono_audio, quantizers);
const auto decoded = decoder.decode(packet, quantizers);
```

Runtime-model API:

```cpp
encodec::encoder encoder("models/encodec-48khz-f32.bin");
encodec::decoder decoder("models/encodec-decoder-48khz-f32.bin");

encodec::set_num_threads(4);
auto frame = encoder.encode_frame(stereo_audio, 2); // 3 kbps at 48 kHz
auto decoded = decoder.decode(frame.packet, 2, frame.code_frames);
std::vector<float> restored(decoded.begin(), decoded.end());

for (float& sample : restored) {
    sample *= frame.scale;
}
```

The packet span returned by an encoder remains owned by that encoder and is
invalidated by its next encode operation.

## Android integration

For a decoder-only Android application:

- export and package only the matching decoder-only model;
- do not package the 65–73 MiB combined model;
- start with one decoder worker and measure speed, temperature and energy on the
  target device;
- ARM64 builds use Eigen's NEON path;
- treat OpenMP as an explicit power/performance tradeoff;
- keep model loading and decoding off the UI thread.

The native model format and API are experimental and may change before a stable
release.

## Tests

```sh
build/encodec_tests
```

The current tests cover the legacy 24 kHz path and partial final codec-hop
padding. Runtime-model smoke tests require separately generated model files and
are therefore not part of the repository test binary.

## Known limitations

- No LM entropy coding.
- WAV input only in the compressor; no automatic resampling.
- No stateful live-streaming API.
- The 24 kHz CLI encodes a file as one continuous causal stream, so long inputs
  require substantial working memory.
- Runtime model files contain Float32 weights and are not committed.

## Attribution and license

This fork builds on [pfeatherstone/encodec.cpp](https://github.com/pfeatherstone/encodec.cpp).
The model architecture and official pretrained checkpoints come from
[Meta EnCodec](https://github.com/facebookresearch/encodec).

The C++ source is licensed under the MIT License; see [LICENSE](LICENSE).
Pretrained model weights may have separate terms. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
