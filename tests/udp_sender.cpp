// SPDX-License-Identifier: GPL-3.0-or-later
#include "rtp.h"
#include <iostream>
#ifdef _WIN32
#include <winsock2.h>
#endif
int main(int argc, char** argv) {
    if (argc != 3) return 2;
#ifdef _WIN32
    WSADATA state; if (WSAStartup(MAKEWORD(2, 2), &state)) return 2;
#endif
    try {
        mini::UdpOutput video("127.0.0.1", uint16_t(std::stoul(argv[1])));
        mini::UdpOutput audio("127.0.0.1", uint16_t(std::stoul(argv[2])));
        mini::RtpStream v(96, 90000, 1200, [&](const auto& b, bool c) { video.send(b, c); });
        mini::RtpStream a(97, 44100, 1200, [&](const auto& b, bool c) { audio.send(b, c); });
        mini::Bytes nal{0,0,0,1,0x65}; nal.insert(nal.end(), 2000, 0xab);
        v.video(nal.data(), nal.size(), false, 1700000000000000000ULL);
        std::vector<int16_t> samples;
        for (int i = 0; i < 2000; ++i) samples.push_back(int16_t(i - 1000));
        a.pcm(samples.data(), 1000, 1700000000000000000ULL);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
#ifdef _WIN32
    WSACleanup();
#endif
}
