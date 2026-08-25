![Ubuntu](https://github.com/pfeatherstone/encodec.cpp/actions/workflows/ubuntu.yml/badge.svg)
![MacOS](https://github.com/pfeatherstone/encodec.cpp/actions/workflows/macos.yml/badge.svg)
![Windows](https://github.com/pfeatherstone/encodec.cpp/actions/workflows/windows.yml/badge.svg)

# encodec.cpp

A C++ implementation of Meta's [Encodec](https://audiocraft.metademolab.com/encodec.html) using [Eigen](https://gitlab.com/libeigen/eigen).

> This branch is an experimental decoder-first fork. It adds runtime-selectable
> 24 kHz mono and 48 kHz stereo models for use by the Android EnCodec player. The
> original 24 kHz API remains available. The 48 kHz encoder, decoder and non-LM
> ECDC CLIs are now available, but Android integration and
> device power benchmarks remain experimental. See
> [docs/DUAL_MODEL_DECODER.md](docs/DUAL_MODEL_DECODER.md).

## API

```cpp
encodec::encoder enc;
encodec::decoder dec;

float audio[24000];
size_t bps = 24000; // 12000, 6000 or 3000
std::span<const uint8_t> packet = enc.encode(audio, bps);
std::span<const float>   audio2 = dec.decode(packet, bps);
```

The runtime-model API can load a decoder-only version-1 model or a combined
encoder/decoder version-2 model:

```cpp
encodec::encoder enc("encodec-48khz-f32.bin");
encodec::decoder dec("encodec-decoder-48khz-f32.bin");
auto frame = enc.encode_frame(stereo_audio, 2); // 3 kbps at 48 kHz
auto audio = dec.decode(frame.packet, 2, frame.code_frames);
// Multiply decoded samples by frame.scale for normalized 48 kHz frames.
```

Export a decoder-only model from an official Meta checkpoint:

```sh
python tools/export_decoder_model.py \
  --checkpoint encodec_48khz-7e698e3e.th \
  --sample-rate 48000 \
  --output encodec-decoder-48khz-f32.bin
```

Export a combined encoder/decoder model for desktop or server-side encoding:

```sh
python tools/export_decoder_model.py \
  --checkpoint encodec_48khz-7e698e3e.th \
  --sample-rate 48000 \
  --include-encoder \
  --output encodec-48khz-f32.bin
```

Encode a 48 kHz stereo WAV into a non-LM ECDC file:

```sh
encodec_compress \
  --model encodec-48khz-f32.bin \
  --input input.wav \
  --output output.ecdc \
  --bandwidth 3
```

Decode a non-LM `.ecdc` file, preserving 48 kHz frame scales and overlap-add:

```sh
encodec_decompress \
  --model encodec-decoder-48khz-f32.bin \
  --input input.ecdc \
  --output output.wav \
  --threads 2 \
  --rescale
```

The ECDC CLI defaults to two independent frame workers. Use `--threads 1` for
the lowest CPU and memory usage. More than two workers can reduce latency on a
desktop, but has diminishing returns and is not recommended as an Android
default without device power measurements.

## Notes

* The legacy 24 kHz weights are compiled into the library. Runtime 24/48 kHz
  weights are loaded from an exported model file.

* You must manually implement streaming for now. Partition your audio into 1s chunks with 10ms overlap. For decoding, use a linear weighting in the overlap regions.

## Features

- [x] Block based API
- [ ] Streaming API

## License

This project is licensed under the MIT License. See the LICENSE file for details.

Pretrained weights downloaded by helper scripts are subject to their own licenses. See THIRD_PARTY_NOTICES.md for details.
