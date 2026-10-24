// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"
#include <iostream>
#include <stdexcept>

void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct BitWriter {
    std::vector<uint8_t> bytes;
    size_t bit = 0;
    void put(uint32_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i, ++bit) {
            if (!(bit % 8)) bytes.push_back(0);
            bytes.back() |= ((value >> (count - 1 - i)) & 1) << (7 - bit % 8);
        }
    }
};
int main() {
    try {
#ifndef MINI_EXPECT_AUDIO
        require(!mini::AudioDecoder::available(2) && !mini::AudioDecoder::available(8), "video-only build loaded an audio codec");
        std::cout << "Audio disabled; no audio codec dependency\n"; return 0;
#endif
        require(mini::AudioDecoder::available(2), "ALAC decoder missing");
        require(mini::AudioDecoder::available(8), "AAC decoder missing");
        mini::AudioDecoder decoder;
        decoder.configure(8, 480); decoder.configure(8, 512); decoder.configure(4, 1024);
        for (unsigned frames : {352u, 480u}) {
            decoder.configure(2, frames);
            std::vector<int16_t> expected;
            BitWriter bits;
            bits.put(1, 3);  // CPE: two-channel element
            bits.put(0, 4); bits.put(0, 12); // tag and reserved bits
            bits.put(0, 1); bits.put(0, 2); bits.put(1, 1); // implicit frame size, uncompressed
            for (unsigned i = 0; i < frames * 2; ++i) {
                const int16_t value = i % 4 == 0 ? -32768 : i % 4 == 1 ? 32767 : int16_t(i * 31 - 4000);
                expected.push_back(value); bits.put(uint16_t(value), 16);
            }
            bits.put(7, 3); // END
            require(decoder.decode(bits.bytes.data(), bits.bytes.size()) == expected, "ALAC decode sample mismatch");
            decoder.flush();
            require(decoder.decode(bits.bytes.data(), bits.bytes.size()) == expected, "ALAC flush/reuse");
        }
        bool rejected = false;
        try { decoder.configure(8, 123); } catch (const std::exception&) { rejected = true; }
        require(rejected, "unsupported AAC-ELD frame size must fail");
        std::cout << "AAC configuration and bit-exact ALAC decoding passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
