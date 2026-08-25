#include <encodec.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 6)
    {
        std::cerr << "Usage: encodec_decode_packet MODEL PACKET NQUANTIZERS CODE_FRAMES OUTPUT_F32\n";
        return 2;
    }
    try
    {
        std::ifstream packet_file(argv[2], std::ios::binary);
        std::vector<uint8_t> packet((std::istreambuf_iterator<char>(packet_file)), {});
        if (!packet_file && !packet_file.eof()) throw std::runtime_error("Cannot read packet");

        std::unique_ptr<encodec::decoder> decoder = std::string_view(argv[1]) == "-"
            ? std::make_unique<encodec::decoder>()
            : std::make_unique<encodec::decoder>(argv[1]);
        const auto info = decoder->info();
        const auto audio = decoder->decode(packet, unsigned(std::stoul(argv[3])), std::stoul(argv[4]));
        std::ofstream output(argv[5], std::ios::binary);
        output.write(reinterpret_cast<const char*>(audio.data()),
                     std::streamsize(audio.size_bytes()));
        if (!output) throw std::runtime_error("Cannot write output");
        std::cout << info.sample_rate << " Hz, " << info.channels << " channel(s), "
                  << audio.size()/info.channels << " frames\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
