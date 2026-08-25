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

class packed_bit_writer
{
public:
    explicit packed_bit_writer(std::ostream& output_) : output{output_} {}

    void append(std::span<const uint8_t> bytes, size_t bit_offset, size_t useful_bits)
    {
        if (bit_offset + useful_bits > bytes.size()*8)
            throw std::runtime_error("Encoded packet is shorter than its code count");
        for (size_t bit = 0; bit < useful_bits; ++bit)
        {
            const size_t source_bit = bit_offset + bit;
            pending |= uint8_t((bytes[source_bit/8] >> (source_bit%8)) & 1u) << pending_bits;
            if (++pending_bits == 8)
            {
                output.put(char(pending));
                pending = 0;
                pending_bits = 0;
            }
        }
    }

    void finish()
    {
        if (pending_bits != 0) output.put(char(pending));
        pending = 0;
        pending_bits = 0;
    }

private:
    std::ostream& output;
    uint8_t pending{};
    unsigned int pending_bits{};
};

struct arguments
{
    std::string model;
    std::string input;
    std::string output;
    double bandwidth_kbps{3.0};
    unsigned int threads{1};
    unsigned int chunk_seconds{};
    unsigned int warmup_seconds{};
    bool chunk_seconds_set{};
    bool warmup_seconds_set{};
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
        else if ((option == "-t" || option == "--threads") && i + 1 < argc)
            args.threads = unsigned(std::stoul(argv[++i]));
        else if (option == "--chunk-seconds" && i + 1 < argc)
        {
            args.chunk_seconds = unsigned(std::stoul(argv[++i]));
            args.chunk_seconds_set = true;
        }
        else if (option == "--warmup-seconds" && i + 1 < argc)
        {
            args.warmup_seconds = unsigned(std::stoul(argv[++i]));
            args.warmup_seconds_set = true;
        }
        else throw std::runtime_error("Unknown or incomplete argument: " + option);
    }
    if (args.model.empty() || args.input.empty() || args.output.empty())
        throw std::runtime_error("Usage: encodec_compress -m MODEL -i INPUT.wav -o OUTPUT.ecdc [-b KBPS] [-t THREADS] [--chunk-seconds N] [--warmup-seconds N]");
    if (!(args.bandwidth_kbps > 0.0)) throw std::runtime_error("Bandwidth must be positive");
    if (args.threads == 0 || args.threads > 16)
        throw std::runtime_error("Thread count must be between 1 and 16");
    if (args.chunk_seconds_set && (args.chunk_seconds == 0 || args.chunk_seconds > 3600))
        throw std::runtime_error("Chunk duration must be between 1 and 3600 seconds");
    if (args.warmup_seconds_set && args.warmup_seconds > 60)
        throw std::runtime_error("Warm-up duration must be between 0 and 60 seconds");
    return args;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const arguments args = parse_arguments(argc, argv);
        const auto started = std::chrono::steady_clock::now();
        encodec::set_num_threads(args.threads);
        const wav_audio audio = read_wav(args.input);
        encodec::encoder encoder(args.model);
        const auto info = encoder.info();
        if (audio.sample_rate != info.sample_rate || audio.channels != info.channels)
            throw std::runtime_error("WAV format does not match the selected model");

        const uint64_t audio_length = audio.samples.size() / info.channels;
        uint64_t segment_length = 48000;
        uint64_t segment_stride = 47520;
        uint64_t warmup_length = 0;
        if (info.sample_rate == 24000)
        {
            const uint64_t duration_seconds =
                (audio_length + info.sample_rate - 1) / info.sample_rate;
            const uint64_t chunk_seconds = args.chunk_seconds_set ? args.chunk_seconds :
                (duration_seconds <= 60 ? std::max<uint64_t>(duration_seconds, 1) : 30);
            segment_length = chunk_seconds * info.sample_rate;
            segment_length -= segment_length % 320;
            segment_stride = segment_length;
            const bool multiple_chunks = audio_length > segment_length;
            const uint64_t warmup_seconds = args.warmup_seconds_set ? args.warmup_seconds :
                (multiple_chunks ? 1 : 0);
            warmup_length = warmup_seconds * info.sample_rate;
            warmup_length -= warmup_length % 320;
            std::cout << "Chunking: " << chunk_seconds << " s (" << segment_length
                      << " samples), warm-up: " << warmup_seconds << " s ("
                      << warmup_length << " samples)\n";
        }
        const double codebook_kbps = (double(info.sample_rate) / 320.0) * 10.0 / 1000.0;
        const unsigned int codebooks = unsigned(std::llround(args.bandwidth_kbps / codebook_kbps));
        if (codebooks == 0 || codebooks > info.max_quantizers)
            throw std::runtime_error("Bandwidth requires an unsupported number of codebooks");

        const std::string model_name = info.sample_rate == 48000 ? "encodec_48khz" : "encodec_24khz";
        std::string metadata = "{\"m\":\"" + model_name + "\",\"al\":" +
            std::to_string(audio_length) + ",\"nc\":" + std::to_string(codebooks) +
            ",\"lm\":false";
        if (info.sample_rate == 24000)
            metadata += ",\"cs\":" + std::to_string(segment_length) +
                        ",\"cw\":" + std::to_string(warmup_length);
        metadata += "}";
        std::ofstream output(args.output, std::ios::binary);
        if (!output) throw std::runtime_error("Cannot create output ECDC: " + args.output);
        output.write("ECDC", 4);
        output.put(0);
        write_be_u32(output, uint32_t(metadata.size()));
        output.write(metadata.data(), std::streamsize(metadata.size()));

        packed_bit_writer continuous_codes(output);
        size_t frame_index = 0;
        for (uint64_t offset = 0; offset < audio_length; offset += segment_stride)
        {
            const size_t frames = size_t(std::min<uint64_t>(segment_length, audio_length - offset));
            uint64_t source_offset = offset;
            uint64_t prefix_samples = 0;
            if (info.sample_rate == 24000 && offset > 0 && warmup_length > 0)
            {
                prefix_samples = std::min(warmup_length, offset);
                prefix_samples -= prefix_samples % 320;
                source_offset -= prefix_samples;
            }
            const auto first = audio.samples.data() + source_offset*info.channels;
            const size_t input_frames = size_t(prefix_samples) + frames;
            const auto encoded = encoder.encode_frame(
                std::span<const float>{first, input_frames*info.channels}, codebooks);
            if (info.normalized)
            {
                write_be_float(output, encoded.scale);
                output.write(reinterpret_cast<const char*>(encoded.packet.data()),
                             std::streamsize(encoded.packet.size()));
            }
            else
            {
                const size_t prefix_code_frames = size_t(prefix_samples / 320);
                const size_t wanted_code_frames = (frames + 319) / 320;
                if (encoded.code_frames != prefix_code_frames + wanted_code_frames)
                    throw std::runtime_error("Unexpected code-frame count while chunking");
                const size_t bit_offset = prefix_code_frames * codebooks * 10;
                const size_t code_bits = wanted_code_frames * codebooks * 10;
                continuous_codes.append(encoded.packet, bit_offset, code_bits);
            }
            if (!output) throw std::runtime_error("Failed while writing ECDC frame");
            std::cerr << "\rEncoded frame " << ++frame_index << std::flush;
        }
        if (!info.normalized) continuous_codes.finish();
        if (!output) throw std::runtime_error("Failed while finalizing ECDC codes");
        std::cerr << '\n';
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "Encoded " << audio_length << " frames at " << info.sample_rate << " Hz, "
                  << info.channels << " channels, " << codebooks << " codebooks ("
                  << codebooks*codebook_kbps << " kbps)\nThreads: " << encodec::get_num_threads()
                  << "\nElapsed: " << elapsed << " s\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
