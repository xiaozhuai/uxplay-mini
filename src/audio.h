// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mini {
// AirPlay currently negotiates 44.1 kHz stereo. No resampler is needed.
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;
    static bool available(unsigned compression_type);
    void configure(unsigned compression_type, unsigned samples_per_frame = 0);
    void flush();
    std::vector<int16_t> decode(const uint8_t* data, size_t size);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
