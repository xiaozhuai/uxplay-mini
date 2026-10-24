// SPDX-License-Identifier: GPL-3.0-or-later
#include "rtp.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#endif

namespace mini {
#ifdef _WIN32
using AddressLength = int;
#else
using AddressLength = socklen_t;
#endif
namespace {
void u16(Bytes& b, uint16_t x) { b.push_back(uint8_t(x >> 8)); b.push_back(uint8_t(x)); }
void u32(Bytes& b, uint32_t x) { u16(b, uint16_t(x >> 16)); u16(b, uint16_t(x)); }
uint32_t random32() { std::random_device rd; return (uint32_t(rd()) << 16) ^ uint32_t(rd()); }
struct Nal { const uint8_t* data; size_t size; };
std::vector<Nal> split(const uint8_t* data, size_t size) {
    // Accept only Annex B. Upstream marks failed decryption with data[0] = 1.
    auto prefix = [&](size_t p) -> size_t {
        if (p + 3 > size || data[p] || data[p + 1]) return 0;
        if (data[p + 2] == 1) return 3;
        return p + 4 <= size && !data[p + 2] && data[p + 3] == 1 ? 4 : 0;
    };
    if (!data || !prefix(0)) return {};
    std::vector<Nal> nals;
    size_t pos = prefix(0);
    while (pos < size) {
        size_t end = pos;
        while (end < size && !prefix(end)) ++end;
        size_t len = end - pos;
        while (len && data[pos + len - 1] == 0) --len; // Annex B trailing_zero_8bits
        if (len) nals.push_back({data + pos, len});
        pos = end < size ? end + prefix(end) : size;
    }
    return nals;
}
}

RtpStream::RtpStream(uint8_t pt, uint32_t rate, size_t mtu, PacketSink sink)
    : pt_(pt), rate_(rate), mtu_(mtu), sink_(std::move(sink)) {
    if (pt > 127 || !rate || mtu < 64 || mtu > 65507) throw std::invalid_argument("invalid RTP configuration");
    reset();
}
void RtpStream::reset() {
    sequence_ = uint16_t(random32()); ssrc_ = random32(); offset_ = random32();
    packets_ = octets_ = 0; last_report_ = 0;
    pcm_clock_valid_ = false;
}
uint32_t RtpStream::timestamp(uint64_t ns, uint32_t rate) {
    // Divide before multiplying to avoid overflow at Unix-epoch-sized times.
    return uint32_t((ns / 1000000000ULL) * rate + ((ns % 1000000000ULL) * rate) / 1000000000ULL);
}
void RtpStream::packet(const uint8_t* data, size_t size, uint32_t ts, bool marker, uint64_t ns) {
    Bytes b; b.reserve(size + 12);
    b.push_back(0x80); b.push_back(pt_ | (marker ? 0x80 : 0));
    u16(b, sequence_++); u32(b, ts); u32(b, ssrc_);
    b.insert(b.end(), data, data + size);
    sink_(b, false); ++packets_; octets_ += uint32_t(size);
    if (!last_report_ || (ns > last_report_ && ns - last_report_ >= 1000000000ULL)) report(ts, ns);
}
void RtpStream::report(uint32_t ts, uint64_t ns) {
    // RFC 3550 compound SR + SDES. Shared CNAME and wall clock synchronize A/V.
    Bytes b{0x80, 200, 0, 6};
    u32(b, ssrc_); u32(b, uint32_t(ns / 1000000000ULL + 2208988800ULL));
    u32(b, uint32_t(((ns % 1000000000ULL) << 32) / 1000000000ULL));
    u32(b, ts); u32(b, packets_); u32(b, octets_);
    constexpr char cname[] = "uxplay-mini";
    Bytes sdes{0x81, 202, 0, 0}; u32(sdes, ssrc_);
    sdes.push_back(1); sdes.push_back(sizeof(cname) - 1);
    sdes.insert(sdes.end(), cname, cname + sizeof(cname)); // includes END item
    while (sdes.size() % 4) sdes.push_back(0);
    sdes[3] = uint8_t(sdes.size() / 4 - 1);
    b.insert(b.end(), sdes.begin(), sdes.end());
    sink_(b, true); last_report_ = ns;
}
void RtpStream::video(const uint8_t* data, size_t size, bool hevc, uint64_t ns) {
    const auto nals = split(data, size);
    const uint32_t ts = timestamp(ns, rate_) + offset_;
    const size_t max_payload = mtu_ - 12;
    for (size_t i = 0; i < nals.size(); ++i) {
        const auto nal = nals[i];
        const size_t header = hevc ? 2 : 1;
        if (nal.size < header) continue;
        const bool last = i + 1 == nals.size();
        if (nal.size <= max_payload) { packet(nal.data, nal.size, ts, last, ns); continue; }
        size_t pos = header;
        while (pos < nal.size) {
            const size_t chunk = std::min(max_payload - (hevc ? 3 : 2), nal.size - pos);
            const bool start = pos == header, end = pos + chunk == nal.size;
            Bytes fu;
            if (hevc) {
                // RFC 7798 FU (49), preserve forbidden bit, layer ID and temporal ID.
                fu = {uint8_t((nal.data[0] & 0x81) | (49 << 1)), nal.data[1],
                      uint8_t(((nal.data[0] >> 1) & 0x3f) | (start ? 0x80 : 0) | (end ? 0x40 : 0))};
            } else {
                // RFC 6184 FU-A (28).
                fu = {uint8_t((nal.data[0] & 0xe0) | 28),
                      uint8_t((nal.data[0] & 0x1f) | (start ? 0x80 : 0) | (end ? 0x40 : 0))};
            }
            fu.insert(fu.end(), nal.data + pos, nal.data + pos + chunk);
            packet(fu.data(), fu.size(), ts, last && end, ns);
            pos += chunk;
        }
    }
}
void RtpStream::pcm(const int16_t* samples, size_t frames, uint64_t ns) {
    pcm_at(samples, frames, ns, timestamp(ns, rate_) + offset_);
}
void RtpStream::pcm(const int16_t* samples, size_t frames, uint64_t ns, uint32_t sample_timestamp) {
    if (!frames) return;
    if (ns || !pcm_clock_valid_) {
        // Arrival time supplies only the initial fallback anchor, never the RTP sample clock.
        pcm_clock_ns_ = ns ? ns : uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        pcm_clock_timestamp_ = sample_timestamp;
        pcm_clock_valid_ = true;
    }
    const uint32_t difference = sample_timestamp - pcm_clock_timestamp_;
    const int64_t delta = difference <= 0x7fffffffU ? int64_t(difference) : int64_t(difference) - 0x100000000LL;
    const int64_t step_ns = delta * 1000000000LL / rate_;
    pcm_clock_ns_ = uint64_t(int64_t(pcm_clock_ns_) + step_ns);
    pcm_clock_timestamp_ = sample_timestamp;
    pcm_at(samples, frames, pcm_clock_ns_, sample_timestamp + offset_);
}
void RtpStream::pcm_at(const int16_t* samples, size_t frames, uint64_t ns, uint32_t ts) {
    const size_t max_frames = (mtu_ - 12) / 4;
    for (size_t pos = 0; pos < frames;) {
        const size_t count = std::min(max_frames, frames - pos);
        Bytes payload; payload.reserve(count * 4);
        for (size_t i = pos * 2; i < (pos + count) * 2; ++i) u16(payload, uint16_t(samples[i]));
        const uint64_t packet_ns = ns + pos * 1000000000ULL / rate_;
        packet(payload.data(), payload.size(), ts + uint32_t(pos),
               packets_ == 0, packet_ns);
        pos += count;
    }
}

struct UdpOutput::Impl {
#ifdef _WIN32
    SOCKET socket = INVALID_SOCKET;
#else
    int socket = -1;
#endif
    sockaddr_storage rtp{}, rtcp{};
    AddressLength length = 0;
};
UdpOutput::UdpOutput(const std::string& host, uint16_t port) : impl_(std::make_unique<Impl>()) {
    if (!port || port == 65535) throw std::invalid_argument("RTP port must be 1..65534");
    addrinfo hints{}, *address = nullptr;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_DGRAM; hints.ai_flags = AI_NUMERICHOST;
    const auto service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &address) || !address)
        throw std::runtime_error("invalid numeric destination address: " + host);
    impl_->socket = ::socket(address->ai_family, SOCK_DGRAM, 0);
    std::memcpy(&impl_->rtp, address->ai_addr, address->ai_addrlen);
    impl_->length = AddressLength(address->ai_addrlen);
    freeaddrinfo(address);
    impl_->rtcp = impl_->rtp;
    if (impl_->rtcp.ss_family == AF_INET) reinterpret_cast<sockaddr_in*>(&impl_->rtcp)->sin_port = htons(port + 1);
    else reinterpret_cast<sockaddr_in6*>(&impl_->rtcp)->sin6_port = htons(port + 1);
#ifdef _WIN32
    if (impl_->socket == INVALID_SOCKET)
#else
    if (impl_->socket < 0)
#endif
        throw std::runtime_error("could not create UDP output socket");
}
UdpOutput::~UdpOutput() {
#ifdef _WIN32
    closesocket(impl_->socket);
#else
    close(impl_->socket);
#endif
}
void UdpOutput::send(const Bytes& b, bool rtcp) {
    const auto& dst = rtcp ? impl_->rtcp : impl_->rtp;
    const auto sent = sendto(impl_->socket, reinterpret_cast<const char*>(b.data()), int(b.size()), 0,
                             reinterpret_cast<const sockaddr*>(&dst), impl_->length);
    if (sent != static_cast<decltype(sent)>(b.size())) throw std::runtime_error("UDP send failed");
}
}
