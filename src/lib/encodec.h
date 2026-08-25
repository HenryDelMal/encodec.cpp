#pragma once

#include <cstdint>
#include <span>
#include <memory>
#include <filesystem>

namespace encodec
{

    struct model_info
    {
        unsigned int sample_rate{};
        unsigned int channels{};
        unsigned int max_quantizers{};
        bool causal{};
        bool normalized{};
    };

//----------------------------------------------------------------------------------------------------------------

    unsigned int get_encodec_bps(unsigned int num_quantizers);
    unsigned int get_encoded_nquantizers(unsigned int bps);

//----------------------------------------------------------------------------------------------------------------

    class encoder
    {
    private:
        struct impl;
        std::unique_ptr<impl> state;
        
    public:
        encoder();
        ~encoder();
        encoder(encoder&& other);
        encoder& operator=(encoder&& other);

        std::span<const uint8_t> encode(std::span<const float> audio, unsigned int num_quantizers);
    };

//----------------------------------------------------------------------------------------------------------------

    class decoder
    {
    private:
        struct impl;
        std::unique_ptr<impl> state;
        
    public:
        decoder();
        explicit decoder(const std::filesystem::path& model_path);
        ~decoder();
        decoder(decoder&& other);
        decoder& operator=(decoder&& other);

        std::span<const float> decode(std::span<const uint8_t> packet, unsigned int num_quantizers);
        std::span<const float> decode(std::span<const uint8_t> packet, unsigned int num_quantizers,
                                      std::size_t code_frames);
        model_info info() const;
    };

//----------------------------------------------------------------------------------------------------------------

}
