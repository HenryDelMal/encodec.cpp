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
    uint64_t chunk_samples{};
    uint64_t warmup_samples{};
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

uint64_t json_unsigned(const std::string& json, const std::string& key, uint64_t fallback)
{
    std::smatch match;
    const std::regex expression("\\\"" + key + "\\\"\\s*:\\s*([0-9]+)");
    return std::regex_search(json, match, expression) ? std::stoull(match[1].str()) : fallback;
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
            unsigned(json_unsigned(json, "nc")), json_boolean(json, "lm", false),
            json_unsigned(json, "cs", 0), json_unsigned(json, "cw", 0)};
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
        else if (option == "-r" || option == "--rescale") args.rescale = true;
        else throw std::runtime_error("Unknown or incomplete argument: " + option);
    }
    if (args.model.empty() || args.input.empty() || args.output.empty())
        throw std::runtime_error("Usage: encodec_decompress -m MODEL -i INPUT.ecdc -o OUTPUT.wav [--threads N] [--chunk-seconds N] [--warmup-seconds N] [--rescale]");
    if (args.threads == 0 || args.threads > 16)
        throw std::runtime_error("Thread count must be between 1 and 16");
    if (args.chunk_seconds_set && (args.chunk_seconds == 0 || args.chunk_seconds > 3600))
        throw std::runtime_error("Chunk duration must be between 1 and 3600 seconds");
    if (args.warmup_seconds_set && args.warmup_seconds > 60)
        throw std::runtime_error("Warm-up duration must be between 0 and 60 seconds");
    return args;
}

struct compressed_frame
{
    uint64_t offset{};
    uint64_t wanted{};
    size_t prefix_samples{};
    size_t code_frames{};
    float scale{1};
    std::vector<uint8_t> packet;
    std::vector<float> decoded;
};

std::vector<uint8_t> extract_packed_bits(std::span<const uint8_t> source,
                                         size_t bit_offset, size_t bit_count)
{
    if (bit_offset + bit_count > source.size()*8)
        throw std::runtime_error("ECDC ended while extracting a 24 kHz chunk");
    std::vector<uint8_t> result((bit_count + 7) / 8, 0);
    for (size_t bit = 0; bit < bit_count; ++bit)
    {
        const size_t source_bit = bit_offset + bit;
        result[bit/8] |= uint8_t((source[source_bit/8] >> (source_bit%8)) & 1u) << (bit%8);
    }
    return result;
}
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

        const uint64_t frame_rate = (info.sample_rate + 319) / 320;
        std::vector<compressed_frame> frames;
        uint64_t segment_length = 48000;
        uint64_t segment_stride = 47520;
        if (info.sample_rate == 48000)
        {
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
                frame.scale = read_be_float(input);
                frame.packet.resize(packet_bytes);
                input.read(reinterpret_cast<char*>(frame.packet.data()), std::streamsize(frame.packet.size()));
                if (!input) throw std::runtime_error("ECDC ended while reading frame " + std::to_string(frames.size() - 1));
            }
        }
        else
        {
            const uint64_t duration_seconds =
                (metadata.audio_length + info.sample_rate - 1) / info.sample_rate;
            const uint64_t automatic_chunk_seconds = duration_seconds <= 60 ?
                std::max<uint64_t>(duration_seconds, 1) :
                std::clamp<uint64_t>(80 / args.threads, 5, 30);
            const uint64_t chunk_seconds = args.chunk_seconds_set ?
                args.chunk_seconds : automatic_chunk_seconds;
            segment_length = chunk_seconds * info.sample_rate;
            segment_length -= segment_length % 320;
            segment_stride = segment_length;
            const bool multiple_chunks = metadata.audio_length > segment_length;
            uint64_t warmup_length = 0;
            if (args.warmup_seconds_set)
                warmup_length = uint64_t(args.warmup_seconds) * info.sample_rate;
            else if (multiple_chunks)
                warmup_length = metadata.warmup_samples != 0 ?
                    metadata.warmup_samples : info.sample_rate;
            warmup_length -= warmup_length % 320;
            std::cout << "Chunking: " << chunk_seconds << " s (" << segment_length
                      << " samples), warm-up: " << double(warmup_length) / info.sample_rate
                      << " s (" << warmup_length << " samples)\n";
            std::vector<uint8_t> continuous_codes{
                std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
            const size_t total_code_frames = size_t((metadata.audio_length + 319) / 320);
            const size_t total_code_bits = total_code_frames * metadata.codebooks * 10;
            if (continuous_codes.size() < (total_code_bits + 7) / 8)
                throw std::runtime_error("ECDC ended while reading 24 kHz codes");

            for (uint64_t offset = 0; offset < metadata.audio_length; offset += segment_stride)
            {
                const uint64_t wanted = std::min(segment_length, metadata.audio_length - offset);
                uint64_t prefix_samples = offset == 0 ? 0 : std::min(warmup_length, offset);
                prefix_samples -= prefix_samples % 320;
                const size_t prefix_code_frames = size_t(prefix_samples / 320);
                const size_t wanted_code_frames = size_t((wanted + 319) / 320);
                const size_t first_code_frame = size_t(offset / 320) - prefix_code_frames;
                const size_t code_frames = prefix_code_frames + wanted_code_frames;
                const size_t bit_offset = first_code_frame * metadata.codebooks * 10;
                const size_t bit_count = code_frames * metadata.codebooks * 10;

                compressed_frame& frame = frames.emplace_back();
                frame.offset = offset;
                frame.wanted = wanted;
                frame.prefix_samples = size_t(prefix_samples);
                frame.code_frames = code_frames;
                frame.packet = extract_packed_bits(continuous_codes, bit_offset, bit_count);
            }
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
        if (info.sample_rate == 24000)
        {
            const size_t correction_frames = size_t(info.sample_rate / 100); // 10 ms
            for (const compressed_frame& frame : frames)
            {
                const size_t decoded_frames = frame.decoded.size() / info.channels;
                if (decoded_frames < frame.prefix_samples)
                    throw std::runtime_error("Decoded chunk is shorter than its warm-up prefix");
                const size_t available = std::min<size_t>(
                    frame.wanted, decoded_frames - frame.prefix_samples);
                std::vector<float> correction(info.channels, 0.0f);
                if (frame.offset > 0 && available > 0)
                    for (size_t channel = 0; channel < info.channels; ++channel)
                        correction[channel] = output[(frame.offset - 1)*info.channels + channel] -
                            frame.decoded[frame.prefix_samples*info.channels + channel];

                const size_t fade = std::min(correction_frames, available);
                for (size_t i = 0; i < available; ++i)
                {
                    const float taper = i < fade && fade > 1 ?
                        1.0f - float(i) / float(fade - 1) : 0.0f;
                    for (size_t channel = 0; channel < info.channels; ++channel)
                        output[(frame.offset + i)*info.channels + channel] =
                            frame.decoded[(frame.prefix_samples + i)*info.channels + channel] +
                            correction[channel] * taper;
                }
            }
        }
        else
        {
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
        }

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
