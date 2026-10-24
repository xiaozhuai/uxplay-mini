// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mini {
using Bytes = std::vector<uint8_t>;
using PacketSink = std::function<void(const Bytes&, bool rtcp)>;

// A stream is owned by one media thread. Audio and video use separate instances.
class RtpStream {
public:
    RtpStream(uint8_t payload_type, uint32_t clock_rate, size_t mtu, PacketSink sink);
    void reset();
    void video(const uint8_t* annex_b, size_t size, bool hevc, uint64_t unix_ns);
    void pcm(const int16_t* samples, size_t frames, uint64_t unix_ns);
    // Preserve the sender's sample clock even when NTP is not available (unix_ns == 0).
    void pcm(const int16_t* samples, size_t frames, uint64_t unix_ns, uint32_t sample_timestamp);
    uint32_t ssrc() const { return ssrc_; }
    static uint32_t timestamp(uint64_t ns, uint32_t rate);
private:
    void packet(const uint8_t* data, size_t size, uint32_t ts, bool marker, uint64_t ns);
    void report(uint32_t ts, uint64_t ns);
    void pcm_at(const int16_t* samples, size_t frames, uint64_t ns, uint32_t ts);
    uint8_t pt_;
    uint32_t rate_, ssrc_ = 0, offset_ = 0, packets_ = 0, octets_ = 0;
    uint16_t sequence_ = 0;
    size_t mtu_;
    uint64_t last_report_ = 0;
    bool pcm_clock_valid_ = false;
    uint32_t pcm_clock_timestamp_ = 0;
    uint64_t pcm_clock_ns_ = 0;
    PacketSink sink_;
};

class UdpOutput {
public:
    UdpOutput(const std::string& numeric_host, uint16_t port);
    ~UdpOutput();
    UdpOutput(const UdpOutput&) = delete;
    UdpOutput& operator=(const UdpOutput&) = delete;
    void send(const Bytes& packet, bool rtcp);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
