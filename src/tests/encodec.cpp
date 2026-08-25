#include <vector>
#include <algorithm>
#include <random>
#include "doctest.h"
#include "encodec.h"

static std::mt19937_64 RAND;

TEST_SUITE("[ENCODEC]") 
{
    TEST_CASE("sizes") 
    {
        constexpr size_t BPS[] = {24000, 12000, 6000, 3000};
        encodec::encoder enc;
        encodec::decoder dec;

        for (size_t b{70} ; b < 75 ; ++b)
        {
            std::vector<float> audio(b*320);
            std::generate(begin(audio), end(audio), [&]{return std::normal_distribution<float>{}(RAND);});

            for (auto bps : BPS)
            {
                auto packet = enc.encode(audio,  encodec::get_encoded_nquantizers(bps));
                auto audio2 = dec.decode(packet, encodec::get_encoded_nquantizers(bps));
                REQUIRE(audio2.size()==audio.size());
            }
        }
    }

    TEST_CASE("encoder pads a partial codec hop")
    {
        encodec::encoder enc;
        encodec::decoder dec;
        constexpr size_t input_frames = 70*320 + 1;
        constexpr unsigned int quantizers = 4;
        std::vector<float> audio(input_frames, 0.0f);

        const auto frame = enc.encode_frame(audio, quantizers);
        REQUIRE(frame.code_frames == 71);
        REQUIRE(frame.scale == 1.0f);
        const auto decoded = dec.decode(frame.packet, quantizers, frame.code_frames);
        REQUIRE(decoded.size() == frame.code_frames*320);
    }
}
