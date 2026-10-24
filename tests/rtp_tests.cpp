// SPDX-License-Identifier: GPL-3.0-or-later
#include "rtp.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

using mini::Bytes;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
uint16_t u16(const Bytes& b, size_t p) { return uint16_t((b.at(p) << 8) | b.at(p + 1)); }
uint32_t u32(const Bytes& b, size_t p) { return (uint32_t(u16(b, p)) << 16) | u16(b, p + 2); }

void video(bool hevc) {
    std::vector<Bytes> packets, control;
    mini::RtpStream stream(hevc ? 98 : 96, 90000, 64, [&](const Bytes& b, bool c) { (c ? control : packets).push_back(b); });
    const Bytes small = hevc ? Bytes{0x40, 1, 0x12} : Bytes{0x67, 0x42, 0x1e};
    Bytes large = hevc ? Bytes{0x26, 1} : Bytes{0x65};
    for (unsigned i = 0; i < 500; ++i) large.push_back(uint8_t(1 + i % 250));
    Bytes input{0,0,0,1}; input.insert(input.end(), small.begin(), small.end());
    input.insert(input.end(), {0,0,1}); input.insert(input.end(), large.begin(), large.end());
    const uint64_t ns = 1700000000123456789ULL;
    stream.video(input.data(), input.size(), hevc, ns);
    require(packets.size() > 3, "video fragmentation was not exercised");
    require(Bytes(packets[0].begin() + 12, packets[0].end()) == small, "small NAL changed");
    Bytes rebuilt;
    for (size_t i = 0; i < packets.size(); ++i) {
        const auto& b = packets[i];
        require(b.size() <= 64 && b[0] == 0x80, "RTP MTU/header");
        require((b[1] & 0x7f) == (hevc ? 98 : 96), "RTP payload type");
        require(bool(b[1] & 0x80) == (i + 1 == packets.size()), "RTP marker must end access unit");
        require(u32(b, 4) == u32(packets[0], 4), "video fragments have different timestamps");
        require(u32(b, 8) == stream.ssrc(), "SSRC changed");
        if (i) require(u16(b, 2) == uint16_t(u16(packets[i - 1], 2) + 1), "sequence gap");
        if (!i) continue;
        const size_t fh = hevc ? 14 : 13;
        require((hevc ? (b[12] >> 1) & 0x3f : b[12] & 0x1f) == (hevc ? 49 : 28), "fragment unit type");
        require(bool(b[fh] & 0x80) == (i == 1), "fragment start bit");
        require(bool(b[fh] & 0x40) == (i + 1 == packets.size()), "fragment end bit");
        if (i == 1) {
            if (hevc) rebuilt = {uint8_t((b[12] & 0x81) | ((b[fh] & 0x3f) << 1)), b[13]};
            else rebuilt = {uint8_t((b[12] & 0xe0) | (b[fh] & 0x1f))};
        }
        rebuilt.insert(rebuilt.end(), b.begin() + fh + 1, b.end());
    }
    require(rebuilt == large, "fragment reassembly did not reproduce NAL");
    require(control.size() == 1, "initial RTCP sender report");
    const auto& sr = control[0];
    require(sr[1] == 200 && u16(sr, 2) == 6, "RTCP SR header");
    require(u32(sr, 4) == stream.ssrc() && u32(sr, 16) == u32(packets[0], 4), "RTCP RTP timestamp mapping");
    require(u32(sr, 8) == uint32_t(ns / 1000000000ULL + 2208988800ULL), "RTCP NTP epoch");
    require(sr[29] == 202 && u16(sr, 30) * 4 + 4 == sr.size() - 28, "compound RTCP SDES length");
    require(sr[36] == 1 && sr[37] == 11 && std::string(sr.begin() + 38, sr.begin() + 49) == "uxplay-mini", "RTCP CNAME");
    const auto first_timestamp = u32(packets[0], 4);
    stream.video(input.data(), input.size(), hevc, ns + 1000000000ULL);
    require(u32(packets[packets.size() / 2], 4) - first_timestamp == 90000, "90 kHz video clock");
    const auto count = packets.size();
    const Bytes invalid{1,0,0,1,0x65,1};
    stream.video(invalid.data(), invalid.size(), hevc, ns);
    stream.video(nullptr, 0, hevc, ns);
    require(packets.size() == count, "invalid decrypted video must be dropped");
}
void audio() {
    std::vector<Bytes> packets;
    mini::RtpStream stream(97, 44100, 64, [&](const Bytes& b, bool c) { if (!c) packets.push_back(b); });
    const std::vector<int16_t> input{0, -1, 32767, -32768, 0x1234, -1234, 1, 2};
    std::vector<int16_t> samples;
    for (int i = 0; i < 100; ++i) samples.insert(samples.end(), input.begin(), input.end());
    stream.pcm(samples.data(), samples.size() / 2, 1700000000000000000ULL);
    Bytes actual;
    size_t frames = 0;
    for (size_t i = 0; i < packets.size(); ++i) {
        const auto& b = packets[i];
        require(b.size() <= 64 && (b.size() - 12) % 4 == 0, "PCM frame boundaries/MTU");
        require((b[1] & 0x7f) == 97 && bool(b[1] & 0x80) == (i == 0), "PCM payload/marker");
        require(u32(b, 4) - u32(packets[0], 4) == frames, "PCM timestamp increments");
        actual.insert(actual.end(), b.begin() + 12, b.end());
        frames += (b.size() - 12) / 4;
    }
    require(frames == samples.size() / 2, "PCM sample loss");
    for (size_t i = 0; i < samples.size(); ++i) require(u16(actual, i * 2) == uint16_t(samples[i]), "L16 byte order/sample value");
}
void wrap() {
    uint16_t previous = 0; unsigned count = 0;
    mini::RtpStream stream(96, 90000, 64, [&](const Bytes& b, bool c) {
        if (c) return;
        if (count++) require(u16(b, 2) == uint16_t(previous + 1), "sequence wraparound");
        previous = u16(b, 2);
    });
    const Bytes nal{0,0,1,0x61,0x80};
    for (int i = 0; i < 65540; ++i) stream.video(nal.data(), nal.size(), false, 1);
    require(count == 65540, "wraparound packet count");
}
void audio_sample_clock() {
    std::vector<Bytes> packets, control;
    mini::RtpStream stream(97, 44100, 1200, [&](const Bytes& b, bool c) { (c ? control : packets).push_back(b); });
    std::vector<int16_t> samples(480 * 2, 1234);
    uint32_t source = 0xffffff00U;
    const uint64_t epoch = 1700000000000000000ULL;
    const uint64_t times[] = {epoch, epoch + 22000000ULL, epoch + 23000000ULL, 0, 0, epoch + 1000000000ULL};
    size_t expected_frames = 0;
    for (auto ns : times) {
        stream.pcm(samples.data(), 480, ns, source);
        source += 480;
    }
    for (const auto& packet : packets) {
        require(uint32_t(u32(packet, 4) - u32(packets.front(), 4)) == expected_frames,
                "arrival/NTP jitter changed the PCM sample clock");
        expected_frames += (packet.size() - 12) / 4;
    }
    require(expected_frames == 6 * 480, "PCM frames lost while source clock wrapped");
    require(control.size() >= 2, "NTP became available without updating RTCP");

    // A completely unsynchronized stream still advances by samples, even in a burst.
    stream.reset(); packets.clear(); control.clear(); source = 123;
    for (int i = 0; i < 1000; ++i) {
        stream.pcm(samples.data(), 480, 0, source);
        source += 480;
    }
    expected_frames = 0;
    for (const auto& packet : packets) {
        require(uint32_t(u32(packet, 4) - u32(packets.front(), 4)) == expected_frames,
                "unsynchronized burst used arrival time instead of sample count");
        expected_frames += (packet.size() - 12) / 4;
    }
    require(control.size() >= 10 && control.size() <= 12, "fallback RTCP clock did not advance by sample count");
    const auto& first = control.front();
    for (const auto& sr : control) {
        const double ntp_delta = double(u32(sr, 8) - u32(first, 8)) +
            (double(u32(sr, 12)) - double(u32(first, 12))) / 4294967296.0;
        const double sample_delta = uint32_t(u32(sr, 16) - u32(first, 16)) / 44100.0;
        require(std::abs(ntp_delta - sample_delta) < 0.00001, "fallback RTCP drifted from the source sample clock");
    }
}
int main() {
    try { video(false); video(true); audio(); wrap(); audio_sample_clock(); std::cout << "RTP/RTCP tests passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
