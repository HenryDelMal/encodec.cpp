![Ubuntu](https://github.com/pfeatherstone/encodec.cpp/actions/workflows/ubuntu.yml/badge.svg)
![MacOS](https://github.com/pfeatherstone/encodec.cpp/actions/workflows/macos.yml/badge.svg)
![Windows](https://github.com/pfeatherstone/encodec.cpp/actions/workflows/windows.yml/badge.svg)

# encodec.cpp

A C++ implementation of Meta's [Encodec](https://audiocraft.metademolab.com/encodec.html) using [Eigen](https://gitlab.com/libeigen/eigen).

> This branch is an experimental decoder-first fork. It is adding runtime-selectable
> 24 kHz mono and 48 kHz stereo models for use by the Android EnCodec player. The
> original 24 kHz API remains available. The 48 kHz decoder and non-LM ECDC CLI
> are now validated against Meta's Python decoder, but Android integration and
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

The experimental runtime-model API is:

```cpp
encodec::decoder dec("encodec-decoder-48khz-f32.bin");
auto info = dec.info();
auto audio = dec.decode(packet, 2, 150); // exact code-frame count avoids padding ambiguity
```

Export a decoder-only model from an official Meta checkpoint:

```sh
python tools/export_decoder_model.py \
  --checkpoint encodec_48khz-7e698e3e.th \
  --sample-rate 48000 \
  --output encodec-decoder-48khz-f32.bin
```

Decode a non-LM `.ecdc` file, preserving 48 kHz frame scales and overlap-add:

```sh
encodec_decompress \
  --model encodec-decoder-48khz-f32.bin \
  --input input.ecdc \
  --output output.wav \
  --rescale
```

## Notes

* The weights are compiled into the library.

* You must manually implement streaming for now. Partition your audio into 1s chunks with 10ms overlap. For decoding, use a linear weighting in the overlap regions.

## Features

- [x] Block based API
- [ ] Streaming API

## License

This project is licensed under the MIT License. See the LICENSE file for details.

Pretrained weights downloaded by helper scripts are subject to their own licenses. See THIRD_PARTY_NOTICES.md for details.
