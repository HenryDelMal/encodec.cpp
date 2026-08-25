#include <encodec.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
struct wav_audio
{
    uint32_t sample_rate{};
    uint16_t channels{};
    std::vector<float> samples;
};

uint16_t read_le_u16(std::istream& input)
{
    uint8_t bytes[2]{};
    input.read(reinterpret_cast<char*>(bytes), 2);
    if (!input) throw std::runtime_error("Truncated WAV file");
    return uint16_t(bytes[0]) | uint16_t(bytes[1]) << 8;
}

uint32_t read_le_u32(std::istream& input)
{
    uint8_t bytes[4]{};
    input.read(reinterpret_cast<char*>(bytes), 4);
    if (!input) throw std::runtime_error("Truncated WAV file");
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 |
           uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

wav_audio read_wav(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open input WAV: " + path);
    char riff[4]{}, wave[4]{};
    input.read(riff, 4);
    (void)read_le_u32(input);
    input.read(wave, 4);
    if (!input || std::string_view(riff, 4) != "RIFF" || std::string_view(wave, 4) != "WAVE")
        throw std::runtime_error("Input is not a RIFF/WAVE file");

    uint16_t format{}, channels{}, bits{};
    uint32_t sample_rate{};
    std::vector<uint8_t> data;
    while (input && (format == 0 || data.empty()))
    {
        char id[4]{};
        input.read(id, 4);
        if (!input) break;
        const uint32_t size = read_le_u32(input);
        const auto next = input.tellg() + std::streamoff(size + (size & 1u));
        if (std::string_view(id, 4) == "fmt ")
        {
            if (size < 16) throw std::runtime_error("Invalid WAV fmt chunk");
            format = read_le_u16(input);
            channels = read_le_u16(input);
            sample_rate = read_le_u32(input);
            (void)read_le_u32(input);
            (void)read_le_u16(input);
            bits = read_le_u16(input);
        }
        else if (std::string_view(id, 4) == "data")
        {
            data.resize(size);
            input.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
            if (!input) throw std::runtime_error("Truncated WAV data chunk");
        }
        input.seekg(next);
    }
    if (channels == 0 || sample_rate == 0 || data.empty())
        throw std::runtime_error("WAV file is missing format or audio data");

    const size_t bytes_per_sample = (bits + 7u) / 8u;
    if (bytes_per_sample == 0 || data.size() % (bytes_per_sample * channels) != 0)
        throw std::runtime_error("Invalid WAV sample alignment");
    wav_audio audio{sample_rate, channels, {}};
    audio.samples.resize(data.size() / bytes_per_sample);
    for (size_t i = 0; i < audio.samples.size(); ++i)
    {
        const uint8_t* p = data.data() + i*bytes_per_sample;
        if (format == 3 && bits == 32)
        {
            const uint32_t word = uint32_t(p[0]) | uint32_t(p[1]) << 8 |
                                  uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
            audio.samples[i] = std::bit_cast<float>(word);
        }
        else if (format == 1 && bits == 16)
        {
            const int16_t value = int16_t(uint16_t(p[0]) | uint16_t(p[1]) << 8);
            audio.samples[i] = float(value) / 32768.0f;
        }
        else if (format == 1 && bits == 24)
        {
            int32_t value = int32_t(uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16);
            if (value & 0x800000) value |= ~0xffffff;
            audio.samples[i] = float(value) / 8388608.0f;
        }
        else if (format == 1 && bits == 32)
        {
            const uint32_t word = uint32_t(p[0]) | uint32_t(p[1]) << 8 |
                                  uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
            audio.samples[i] = float(int32_t(word)) / 2147483648.0f;
        }
        else
            throw std::runtime_error("Only PCM16/24/32 and Float32 WAV input is supported");
    }
    return audio;
}

void write_be_u32(std::ostream& output, uint32_t value)
{
    output.put(char(value >> 24));
    output.put(char(value >> 16));
    output.put(char(value >> 8));
    output.put(char(value));
}

void write_be_float(std::ostream& output, float value)
{
    write_be_u32(output, std::bit_cast<uint32_t>(value));
}

struct arguments
{
    std::string model;
    std::string input;
    std::string output;
    double bandwidth_kbps{3.0};
};

arguments parse_arguments(int argc, char** argv)
{
    arguments args;
    for (int i = 1; i < argc; ++i)
    {
        const std::string option = argv[i];
        if ((option == "-m" || option == "--model") && i + 1 < argc) args.model = argv[++i];
        else if ((option == "-i" || option == "--input") && i + 1 < argc) args.input = argv[++i];
        else if ((option == "-o" || option == "--output") && i + 1 < argc) args.output = argv[++i];
        else if ((option == "-b" || option == "--bandwidth") && i + 1 < argc)
            args.bandwidth_kbps = std::stod(argv[++i]);
        else throw std::runtime_error("Unknown or incomplete argument: " + option);
    }
    if (args.model.empty() || args.input.empty() || args.output.empty())
        throw std::runtime_error("Usage: encodec_compress -m MODEL -i INPUT.wav -o OUTPUT.ecdc [-b KBPS]");
    if (!(args.bandwidth_kbps > 0.0)) throw std::runtime_error("Bandwidth must be positive");
    return args;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const arguments args = parse_arguments(argc, argv);
        const auto started = std::chrono::steady_clock::now();
        const wav_audio audio = read_wav(args.input);
        encodec::encoder encoder(args.model);
        const auto info = encoder.info();
        if (audio.sample_rate != info.sample_rate || audio.channels != info.channels)
            throw std::runtime_error("WAV format does not match the selected model");

        const uint64_t audio_length = audio.samples.size() / info.channels;
        const uint64_t segment_length = info.sample_rate == 48000 ? 48000 : audio_length;
        const uint64_t segment_stride = info.sample_rate == 48000 ? 47520 : audio_length;
        const double codebook_kbps = (double(info.sample_rate) / 320.0) * 10.0 / 1000.0;
        const unsigned int codebooks = unsigned(std::llround(args.bandwidth_kbps / codebook_kbps));
        if (codebooks == 0 || codebooks > info.max_quantizers)
            throw std::runtime_error("Bandwidth requires an unsupported number of codebooks");

        const std::string model_name = info.sample_rate == 48000 ? "encodec_48khz" : "encodec_24khz";
        const std::string metadata = "{\"m\":\"" + model_name + "\",\"al\":" +
            std::to_string(audio_length) + ",\"nc\":" + std::to_string(codebooks) +
            ",\"lm\":false}";
        std::ofstream output(args.output, std::ios::binary);
        if (!output) throw std::runtime_error("Cannot create output ECDC: " + args.output);
        output.write("ECDC", 4);
        output.put(0);
        write_be_u32(output, uint32_t(metadata.size()));
        output.write(metadata.data(), std::streamsize(metadata.size()));

        size_t frame_index = 0;
        for (uint64_t offset = 0; offset < audio_length; offset += segment_stride)
        {
            const size_t frames = size_t(std::min<uint64_t>(segment_length, audio_length - offset));
            const auto first = audio.samples.data() + offset*info.channels;
            const auto encoded = encoder.encode_frame(
                std::span<const float>{first, frames*info.channels}, codebooks);
            if (info.normalized) write_be_float(output, encoded.scale);
            output.write(reinterpret_cast<const char*>(encoded.packet.data()),
                         std::streamsize(encoded.packet.size()));
            if (!output) throw std::runtime_error("Failed while writing ECDC frame");
            std::cerr << "\rEncoded frame " << ++frame_index << std::flush;
        }
        std::cerr << '\n';
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "Encoded " << audio_length << " frames at " << info.sample_rate << " Hz, "
                  << info.channels << " channels, " << codebooks << " codebooks ("
                  << codebooks*codebook_kbps << " kbps)\nElapsed: " << elapsed << " s\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
