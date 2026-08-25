#include <encodec.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
struct ecdc_metadata
{
    std::string model;
    uint64_t audio_length{};
    unsigned int codebooks{};
    bool lm{};
};

uint32_t read_be_u32(std::istream& input)
{
    uint8_t bytes[4]{};
    input.read(reinterpret_cast<char*>(bytes), 4);
    if (!input) throw std::runtime_error("Unexpected end of ECDC header");
    return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 |
           uint32_t(bytes[2]) << 8 | uint32_t(bytes[3]);
}

float read_be_float(std::istream& input)
{
    const uint32_t bits = read_be_u32(input);
    return std::bit_cast<float>(bits);
}

std::string json_string(const std::string& json, const std::string& key)
{
    std::smatch match;
    const std::regex expression("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
    if (!std::regex_search(json, match, expression))
        throw std::runtime_error("Missing ECDC metadata field: " + key);
    return match[1].str();
}

uint64_t json_unsigned(const std::string& json, const std::string& key)
{
    std::smatch match;
    const std::regex expression("\\\"" + key + "\\\"\\s*:\\s*([0-9]+)");
    if (!std::regex_search(json, match, expression))
        throw std::runtime_error("Missing ECDC metadata field: " + key);
    return std::stoull(match[1].str());
}

bool json_boolean(const std::string& json, const std::string& key, bool fallback)
{
    std::smatch match;
    const std::regex expression("\\\"" + key + "\\\"\\s*:\\s*(true|false)");
    return std::regex_search(json, match, expression) ? match[1].str() == "true" : fallback;
}

ecdc_metadata read_header(std::istream& input)
{
    char magic[4]{};
    input.read(magic, 4);
    const int version = input.get();
    const uint32_t metadata_size = read_be_u32(input);
    if (std::string_view(magic, 4) != "ECDC") throw std::runtime_error("Not an ECDC file");
    if (version != 0) throw std::runtime_error("Unsupported ECDC version");
    if (metadata_size > 1024 * 1024) throw std::runtime_error("Unreasonable ECDC metadata size");
    std::string json(metadata_size, '\0');
    input.read(json.data(), std::streamsize(json.size()));
    if (!input) throw std::runtime_error("Truncated ECDC metadata");
    return {json_string(json, "m"), json_unsigned(json, "al"),
            unsigned(json_unsigned(json, "nc")), json_boolean(json, "lm", false)};
}

void write_u16(std::ostream& output, uint16_t value)
{
    output.put(char(value & 0xff));
    output.put(char(value >> 8));
}

void write_u32(std::ostream& output, uint32_t value)
{
    for (unsigned int shift = 0; shift < 32; shift += 8) output.put(char(value >> shift));
}

void write_float_wav(const std::string& path, const std::vector<float>& audio,
                     uint32_t sample_rate, uint16_t channels)
{
    const uint64_t data_bytes_64 = audio.size() * sizeof(float);
    if (data_bytes_64 > std::numeric_limits<uint32_t>::max() - 36)
        throw std::runtime_error("Output is too large for a RIFF WAV file");
    const uint32_t data_bytes = uint32_t(data_bytes_64);
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("Cannot create output WAV: " + path);
    output.write("RIFF", 4); write_u32(output, 36 + data_bytes); output.write("WAVE", 4);
    output.write("fmt ", 4); write_u32(output, 16); write_u16(output, 3); // IEEE float
    write_u16(output, channels); write_u32(output, sample_rate);
    write_u32(output, sample_rate * channels * sizeof(float));
    write_u16(output, uint16_t(channels * sizeof(float))); write_u16(output, 32);
    output.write("data", 4); write_u32(output, data_bytes);
    output.write(reinterpret_cast<const char*>(audio.data()), std::streamsize(data_bytes));
    if (!output) throw std::runtime_error("Failed while writing output WAV");
}

struct arguments
{
    std::string model;
    std::string input;
    std::string output;
    bool rescale{};
    unsigned int threads{2};
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
        else if ((option == "-t" || option == "--threads") && i + 1 < argc)
            args.threads = unsigned(std::stoul(argv[++i]));
        else if (option == "-r" || option == "--rescale") args.rescale = true;
        else throw std::runtime_error("Unknown or incomplete argument: " + option);
    }
    if (args.model.empty() || args.input.empty() || args.output.empty())
        throw std::runtime_error("Usage: encodec_decompress -m MODEL -i INPUT.ecdc -o OUTPUT.wav [--threads N] [--rescale]");
    if (args.threads == 0 || args.threads > 16)
        throw std::runtime_error("Thread count must be between 1 and 16");
    return args;
}

struct compressed_frame
{
    uint64_t offset{};
    uint64_t wanted{};
    size_t code_frames{};
    float scale{1};
    std::vector<uint8_t> packet;
    std::vector<float> decoded;
};
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const arguments args = parse_arguments(argc, argv);
        const auto started = std::chrono::steady_clock::now();
        std::ifstream input(args.input, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot open input ECDC: " + args.input);
        const ecdc_metadata metadata = read_header(input);
        if (metadata.lm) throw std::runtime_error("LM-compressed ECDC is not supported");

        encodec::decoder decoder(args.model);
        const auto info = decoder.info();
        const std::string expected_model = info.sample_rate == 48000 ? "encodec_48khz" : "encodec_24khz";
        if (metadata.model != expected_model) throw std::runtime_error("ECDC and decoder model do not match");
        if (metadata.codebooks == 0 || metadata.codebooks > info.max_quantizers)
            throw std::runtime_error("Unsupported ECDC codebook count");

        const uint64_t segment_length = info.sample_rate == 48000 ? 48000 : metadata.audio_length;
        const uint64_t segment_stride = info.sample_rate == 48000 ? 47520 : metadata.audio_length;
        const uint64_t frame_rate = (info.sample_rate + 319) / 320;
        std::vector<compressed_frame> frames;
        for (uint64_t offset = 0; offset < metadata.audio_length; offset += segment_stride)
        {
            const uint64_t wanted = std::min(segment_length, metadata.audio_length - offset);
            const size_t code_frames = size_t((wanted * frame_rate + info.sample_rate - 1) / info.sample_rate);
            const size_t code_count = code_frames * metadata.codebooks;
            const size_t packet_bytes = (code_count * 10 + 7) / 8;
            compressed_frame& frame = frames.emplace_back();
            frame.offset = offset;
            frame.wanted = wanted;
            frame.code_frames = code_frames;
            frame.scale = info.sample_rate == 48000 ? read_be_float(input) : 1.0f;
            frame.packet.resize(packet_bytes);
            input.read(reinterpret_cast<char*>(frame.packet.data()), std::streamsize(frame.packet.size()));
            if (!input) throw std::runtime_error("ECDC ended while reading frame " + std::to_string(frames.size() - 1));
        }

        const unsigned int worker_count = std::min<unsigned int>(args.threads, unsigned(frames.size()));
        std::vector<std::unique_ptr<encodec::decoder>> decoders;
        decoders.reserve(worker_count);
        decoders.push_back(std::make_unique<encodec::decoder>(std::move(decoder)));
        for (unsigned int worker = 1; worker < worker_count; ++worker)
            decoders.push_back(std::make_unique<encodec::decoder>(args.model));

        std::atomic_size_t next_frame{0};
        std::atomic_size_t completed{0};
        std::exception_ptr worker_error;
        std::mutex error_mutex;
        auto decode_frames = [&](unsigned int worker)
        {
            try
            {
                while (true)
                {
                    const size_t index = next_frame.fetch_add(1);
                    if (index >= frames.size()) break;
                    compressed_frame& frame = frames[index];
                    const auto samples = decoders[worker]->decode(
                        frame.packet, metadata.codebooks, frame.code_frames);
                    frame.decoded.assign(samples.begin(), samples.end());
                    for (float& sample : frame.decoded) sample *= frame.scale;
                    const size_t done = completed.fetch_add(1) + 1;
                    if (worker == 0 || done == frames.size())
                        std::cerr << "\rDecoded frame " << done << '/' << frames.size() << std::flush;
                }
            }
            catch (...)
            {
                std::lock_guard lock(error_mutex);
                if (!worker_error) worker_error = std::current_exception();
                next_frame = frames.size();
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(worker_count > 0 ? worker_count - 1 : 0);
        for (unsigned int worker = 1; worker < worker_count; ++worker)
            workers.emplace_back(decode_frames, worker);
        decode_frames(0);
        for (auto& worker : workers) worker.join();
        if (worker_error) std::rethrow_exception(worker_error);
        std::cerr << '\n';

        std::vector<float> output(metadata.audio_length * info.channels, 0.0f);
        std::vector<float> weights(metadata.audio_length, 0.0f);
        for (const compressed_frame& frame : frames)
        {
            const size_t decoded_frames = frame.decoded.size() / info.channels;
            const size_t available = std::min<size_t>(frame.wanted, decoded_frames);
            for (size_t i = 0; i < available; ++i)
            {
                const float t = float(i + 1) / float(segment_length + 1);
                const float weight = 0.5f - std::fabs(t - 0.5f);
                weights[frame.offset + i] += weight;
                for (size_t channel = 0; channel < info.channels; ++channel)
                    output[(frame.offset + i)*info.channels + channel] +=
                        frame.decoded[i*info.channels + channel] * weight;
            }
        }
        for (size_t i = 0; i < metadata.audio_length; ++i)
            if (weights[i] > 0)
                for (size_t channel = 0; channel < info.channels; ++channel)
                    output[i*info.channels + channel] /= weights[i];

        float peak = 0;
        for (float value : output) peak = std::max(peak, std::fabs(value));
        if (args.rescale && peak > 0.99f)
        {
            const float gain = 0.99f / peak;
            for (float& value : output) value *= gain;
            std::cerr << "Rescaled peak " << peak << " with gain " << gain << '\n';
            peak = 0.99f;
        }
        else if (peak > 0.99f)
            std::cerr << "Warning: peak " << peak << " exceeds 0.99; use --rescale\n";

        write_float_wav(args.output, output, info.sample_rate, uint16_t(info.channels));
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "Decoded " << metadata.audio_length << " frames at " << info.sample_rate
                  << " Hz, " << info.channels << " channels, " << metadata.codebooks << " codebooks\n"
                  << "Workers: " << worker_count << "\nPeak: " << peak << "\nElapsed: " << elapsed << " s\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
