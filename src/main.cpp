// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"
#include "rtp.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
extern "C" {
#include "raop.h"
#include "netutils.h"
}
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#endif

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
std::mutex log_mutex;
void log(const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cerr << message << std::endl;
}
struct Options {
    std::string name = "UxPlay Mini", host = "127.0.0.1", keyfile, sdp, password;
    std::array<unsigned char, 6> device{0x02, 0x55, 0x58, 0x50, 0x4c, 0x59};
    unsigned video_port = 5000, audio_port = 5002, port = 0, fps = 60, mtu = 1200, seconds = 0;
    bool audio = true, mdns = true, nohold = true, debug = false, h265 = false, show_help = false;
};
unsigned number(const std::string& value, unsigned min, unsigned max) {
    unsigned result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result < min || result > max)
        throw std::invalid_argument("number out of range: " + value);
    return result;
}
void help() {
    std::cout << "uxplay-mini 0.1.1 - AirPlay to RTP, without GStreamer\n"
        "  -n, --name NAME        AirPlay receiver name (default: UxPlay Mini)\n"
        "  --host ADDRESS         numeric RTP destination, IPv4 or IPv6 (127.0.0.1)\n"
        "  --video-port PORT      video RTP port (5000), RTCP at PORT+1\n"
        "  --audio-port PORT      L16 audio RTP port (5002), RTCP at PORT+1\n"
        "  --port PORT            incoming AirPlay TCP control port (0 = automatic)\n"
        "  --fps N                maximum mirror frame rate, 1..120 (60)\n"
        "  --h265                 allow H265/HEVC video (default: H264 only)\n"
        "  --mtu BYTES            maximum outgoing RTP datagram size (1200)\n"
        "  --device-id XX:XX:XX:XX:XX:XX  unique, stable receiver identity\n"
        "  --keyfile PATH         persistent pairing key (optional)\n"
        "  --password PASSWORD    fixed AirPlay access password (disabled by default)\n"
        "  --sdp PATH             write receiver SDP for the outgoing streams\n"
        "  --no-audio             disable audio decoding and RTP output\n"
        "  --no-mdns              do not advertise services (diagnostic use)\n"
        "  --hold                 reject a second client while connected\n"
        "  -d, --debug            verbose protocol log\n"
        "  --run-seconds N        stop after N seconds (diagnostic use)\n"
        "  -h, --help             show this help\n"
        "Video: H264/90000 PT=96; with --h265: H265/90000 PT=98. Audio: L16/44100/2 PT=97.\n";
}
Options parse(int argc, char** argv) {
    Options o;
#ifndef MINI_WITH_AUDIO
    o.audio = false;
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (++i == argc) throw std::invalid_argument("missing value for " + arg);
            return argv[i];
        };
        if (arg == "--help" || arg == "-h") o.show_help = true;
        else if (arg == "--name" || arg == "-n") o.name = value();
        else if (arg == "--host") o.host = value();
        else if (arg == "--video-port") o.video_port = number(value(), 1, 65534);
        else if (arg == "--audio-port") o.audio_port = number(value(), 1, 65534);
        else if (arg == "--port") o.port = number(value(), 0, 65535);
        else if (arg == "--fps") o.fps = number(value(), 1, 120);
        else if (arg == "--h265") o.h265 = true;
        else if (arg == "--mtu") o.mtu = number(value(), 64, 65507);
        else if (arg == "--run-seconds") o.seconds = number(value(), 1, 86400);
        else if (arg == "--keyfile") o.keyfile = value();
        else if (arg == "--password") {
            o.password = value();
            if (o.password.empty() || o.password.size() > 64 ||
                std::any_of(o.password.begin(), o.password.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
                throw std::invalid_argument("password must contain 1..64 bytes without control characters");
        }
        else if (arg == "--sdp") o.sdp = value();
        else if (arg == "--no-audio") o.audio = false;
        else if (arg == "--no-mdns") o.mdns = false;
        else if (arg == "--hold") o.nohold = false;
        else if (arg == "--debug" || arg == "-d") o.debug = true;
        else if (arg == "--device-id") {
            const auto id = value();
            if (id.size() != 17) throw std::invalid_argument("invalid device ID");
            for (size_t j = 0; j < o.device.size(); ++j) {
                unsigned n = 0;
                const auto p = std::from_chars(id.data() + j * 3, id.data() + j * 3 + 2, n, 16);
                if (p.ec != std::errc{} || p.ptr != id.data() + j * 3 + 2 || (j < 5 && id[j * 3 + 2] != ':'))
                    throw std::invalid_argument("invalid device ID");
                o.device[j] = static_cast<unsigned char>(n);
            }
        } else throw std::invalid_argument("unknown option: " + arg);
    }
    if (o.name.empty() || o.name.size() > 63 || o.name.find_first_of("\r\n") != std::string::npos)
        throw std::invalid_argument("receiver name must contain 1..63 bytes without line breaks");
    if (o.audio && (o.video_port <= o.audio_port + 1 && o.audio_port <= o.video_port + 1))
        throw std::invalid_argument("audio and video RTP/RTCP port pairs overlap");
    return o;
}
void write_sdp(const Options& o) {
    if (o.sdp.empty()) return;
    std::ofstream out(o.sdp, std::ios::binary);
    if (!out) throw std::runtime_error("could not open SDP output: " + o.sdp);
    const char* family = o.host.find(':') == std::string::npos ? "IP4" : "IP6";
    out << "v=0\r\no=- 0 0 IN " << family << ' ' << o.host << "\r\ns=UxPlay Mini\r\nc=IN "
        << family << ' ' << o.host << "\r\nt=0 0\r\n"
        << "m=video " << o.video_port << " RTP/AVP 96" << (o.h265 ? " 98" : "") << "\r\n"
        << "a=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1\r\n";
    if (o.h265) out << "a=rtpmap:98 H265/90000\r\n";
    if (o.audio) out << "m=audio " << o.audio_port << " RTP/AVP 97\r\na=rtpmap:97 L16/44100/2\r\n";
    if (!out) throw std::runtime_error("could not write SDP output: " + o.sdp);
}

struct DnsDelete {
    void operator()(dnssd_t* p) const { dnssd_unregister_raop(p); dnssd_unregister_airplay(p); dnssd_destroy(p); }
};
using Dns = std::unique_ptr<dnssd_t, DnsDelete>;
using Server = std::unique_ptr<raop_t, decltype(&raop_destroy)>;

struct Receiver {
    Options options;
    mini::UdpOutput video_output;
    std::unique_ptr<mini::UdpOutput> audio_output;
    mini::RtpStream h264, h265, pcm;
    mini::AudioDecoder decoder;
    std::mutex audio_mutex;
    std::atomic<bool> video_reset{true}, restart{false};
    unsigned audio_ct = 0;
    double volume = 1.0;
    uint64_t audio_errors = 0, video_errors = 0;
    bool audio_clock_logged = false;
    Dns dns;
    Server server{nullptr, raop_destroy}; // destroyed first, joining media threads

    explicit Receiver(Options o) : options(std::move(o)), video_output(options.host, uint16_t(options.video_port)),
        h264(96, 90000, options.mtu, [this](const auto& b, bool c) { video_output.send(b, c); }),
        h265(98, 90000, options.mtu, [this](const auto& b, bool c) { video_output.send(b, c); }),
        pcm(97, 44100, options.mtu, [this](const auto& b, bool c) { audio_output->send(b, c); }) {
        if (options.audio) {
            if (!mini::AudioDecoder::available(2) || !mini::AudioDecoder::available(8))
                throw std::runtime_error("FFmpeg must include AAC and ALAC decoders; use --no-audio for video only");
            audio_output = std::make_unique<mini::UdpOutput>(options.host, uint16_t(options.audio_port));
        }
        int error = 0;
        dns.reset(dnssd_init(options.name.c_str(), int(options.name.size()),
                            reinterpret_cast<const char*>(options.device.data()), 6, options.password.empty() ? 0 : 2, &error));
        if (!dns) throw std::runtime_error("could not initialize service discovery: " + std::to_string(error));
        // Match upstream's mirror/audio feature set, without HLS or legacy video playback.
        dnssd_set_airplay_features(dns.get(), 0, 0);
        dnssd_set_airplay_features(dns.get(), 4, 0);
        dnssd_set_airplay_features(dns.get(), 42, int(options.h265)); // SupportsScreenMultiCodec
        // Match upstream -pw: fixed passwords use SETUP Digest authentication.
        // Advertising bit 27 makes iPhone request a separate random PIN via SRP.
        if (!options.password.empty()) dnssd_set_airplay_features(dns.get(), 27, 0);
        // iPhone needs the normal audio capabilities to complete mirror setup,
        // even for video-only output. Removing them leaves it connecting until
        // it tears down the session after about a minute. --no-audio (including
        // builds without FFmpeg) only discards audio in the callbacks below;
        // keep accepting the protocol's audio setup and packets.
        start_server();
        write_sdp(options);
        log("Video RTP -> " + options.host + ":" + std::to_string(options.video_port));
        if (options.audio) log("Audio L16 RTP -> " + options.host + ":" + std::to_string(options.audio_port));
    }
    void start_server() {
        raop_callbacks_t cb{};
        cb.cls = this;
        if (!options.password.empty()) {
            cb.passwd = [](void* cls, int* len) {
                const auto& password = static_cast<Receiver*>(cls)->options.password;
                *len = int(password.size());
                return password.c_str();
            };
        }
        cb.audio_process = [](void* cls, raop_ntp_t*, audio_decode_struct* data) {
            auto& r = *static_cast<Receiver*>(cls);
            if (!r.options.audio || data->data_len <= 0) return;
            std::lock_guard<std::mutex> lock(r.audio_mutex);
            try {
                if (r.audio_ct != data->ct) { r.decoder.configure(data->ct); r.audio_ct = data->ct; r.pcm.reset(); }
                auto samples = r.decoder.decode(data->data, size_t(data->data_len));
                if (!r.audio_clock_logged && !samples.empty()) {
                    log(std::string("Audio clock: source RTP at 44100 Hz; reference=") +
                        (data->ntp_time_local ? "NTP" : "local sample-clock anchor (NTP unavailable)"));
                    r.audio_clock_logged = true;
                }
                if (r.volume != 1.0) for (auto& v : samples) v = int16_t(std::lround(v * r.volume));
                r.pcm.pcm(samples.data(), samples.size() / 2, data->ntp_time_local, data->rtp_time);
            } catch (const std::exception& e) { if (++r.audio_errors % 100 == 1) log(std::string("Audio: ") + e.what()); }
        };
        cb.video_process = [](void* cls, raop_ntp_t*, video_decode_struct* data) {
            auto& r = *static_cast<Receiver*>(cls);
            if (data->data_len <= 0) return;
            try {
                if (r.video_reset.exchange(false)) { r.h264.reset(); r.h265.reset(); }
                (data->is_h265 ? r.h265 : r.h264).video(data->data, size_t(data->data_len), data->is_h265,
                    data->ntp_time_local ? data->ntp_time_local : raop_ntp_get_local_time());
            } catch (const std::exception& e) { if (++r.video_errors % 100 == 1) log(std::string("Video: ") + e.what()); }
        };
        cb.video_set_codec_and_size = [](void* cls, video_codec_t codec, unsigned short, unsigned short,
                                       unsigned short, unsigned short, unsigned short width, unsigned short height) {
            const auto& r = *static_cast<Receiver*>(cls);
            if (codec == VIDEO_CODEC_H265 && !r.options.h265) {
                log("H265 is disabled; start with --h265 to enable HEVC video");
                return -1;
            }
            if (codec != VIDEO_CODEC_H264 && codec != VIDEO_CODEC_H265) return -1;
            log(std::string("Video codec: ") + (codec == VIDEO_CODEC_H265 ? "H265 " : "H264 ") +
                std::to_string(width) + "x" + std::to_string(height));
            return 0;
        };
        cb.video_reset = [](void* cls, reset_type_t) { static_cast<Receiver*>(cls)->video_reset = true; log("Mirror session ended"); };
        cb.video_pause = [](void*) {};
        cb.video_resume = [](void*) {};
        cb.conn_feedback = [](void*) {};
        cb.conn_reset = [](void* cls, int) { static_cast<Receiver*>(cls)->restart = true; };
        cb.audio_set_client_volume = [](void*) { return 0.0; };
        cb.audio_set_volume = [](void* cls, float db) {
            auto& r = *static_cast<Receiver*>(cls);
            std::lock_guard<std::mutex> lock(r.audio_mutex);
            r.volume = std::isfinite(db) && db > -144 ? std::pow(10.0, std::min(0.0, double(db)) / 20.0) : 0.0;
        };
        cb.audio_get_format = [](void* cls, unsigned char* ct, unsigned short* spf, bool*, bool*, uint64_t*) {
            auto& r = *static_cast<Receiver*>(cls);
            if (!r.options.audio) return;
            std::lock_guard<std::mutex> lock(r.audio_mutex);
            r.audio_ct = 0;
            r.audio_clock_logged = false;
            log("Audio format: ct=" + std::to_string(*ct) + " samples_per_frame=" + std::to_string(*spf));
            try { r.decoder.configure(*ct, *spf); r.audio_ct = *ct; r.pcm.reset(); }
            catch (const std::exception& e) { log(std::string("Audio format: ") + e.what()); }
        };
        cb.audio_flush = [](void* cls) {
            auto& r = *static_cast<Receiver*>(cls);
            std::lock_guard<std::mutex> lock(r.audio_mutex); r.decoder.flush(); r.pcm.reset();
        };
        cb.video_flush = [](void* cls) { static_cast<Receiver*>(cls)->video_reset = true; };
        cb.display_pin = [](void*, char* pin) { log(std::string("AirPlay PIN: ") + pin); };
        server.reset(raop_init(&cb));
        if (!server) throw std::runtime_error("could not initialize AirPlay");
        raop_set_log_level(server.get(), options.debug ? LOGGER_DEBUG : LOGGER_INFO);
        raop_set_log_callback(server.get(), [](void*, int, const char* msg) { log(msg); }, nullptr);
        std::ostringstream id;
        for (size_t i = 0; i < options.device.size(); ++i) {
            if (i) id << ':';
            id << std::hex << std::setfill('0') << std::setw(2) << unsigned(options.device[i]);
        }
        if (raop_init2(server.get(), options.nohold, id.str().c_str(), options.keyfile.c_str()))
            throw std::runtime_error("could not initialize AirPlay pairing/server");
        raop_set_dnssd(server.get(), dns.get());
        raop_set_plist(server.get(), "maxFPS", int(options.fps));
        unsigned short port = uint16_t(options.port);
        if (raop_start_httpd(server.get(), &port) < 0) throw std::runtime_error("could not bind AirPlay TCP port");
        raop_set_port(server.get(), port);
        if (options.mdns && (dnssd_register_raop(dns.get(), port) || dnssd_register_airplay(dns.get(), port)))
            throw std::runtime_error("could not advertise AirPlay services");
        log("AirPlay receiver \"" + options.name + "\" listening on TCP " + std::to_string(port));
    }
    void run() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.seconds);
        while (!stopping) {
            if (options.seconds && std::chrono::steady_clock::now() >= deadline) break;
            if (restart.exchange(false)) {
                dnssd_unregister_raop(dns.get()); dnssd_unregister_airplay(dns.get());
                server.reset(); video_reset = true;
                { std::lock_guard<std::mutex> lock(audio_mutex); audio_ct = 0; decoder.flush(); }
                start_server();
            }
            if (!raop_is_running(server.get())) throw std::runtime_error("AirPlay control server stopped");
            // Bonjour daemon replies must be serviced; the bundled mDNS backend owns its thread.
            fd_set fds; FD_ZERO(&fds); int max_fd = -1;
            for (int service : {DNSSD_SERVICE_RAOP, DNSSD_SERVICE_AIRPLAY}) {
                const int fd = dnssd_get_service_fd(dns.get(), service);
                if (fd >= 0 && fd < FD_SETSIZE) { FD_SET(fd, &fds); max_fd = std::max(max_fd, fd); }
            }
            if (max_fd < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
            timeval wait{0, 100000};
            if (select(max_fd + 1, &fds, nullptr, nullptr, &wait) > 0) {
                for (int service : {DNSSD_SERVICE_RAOP, DNSSD_SERVICE_AIRPLAY}) {
                    const int fd = dnssd_get_service_fd(dns.get(), service);
                    if (fd >= 0 && fd < FD_SETSIZE && FD_ISSET(fd, &fds) && dnssd_process_service(dns.get(), service))
                        throw std::runtime_error("service discovery disconnected");
                }
            }
        }
        log("Stopping AirPlay receiver");
    }
};
}

int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        if (options.show_help) { help(); return 0; }
        // raop_init also owns one network reference; this one covers UDP output lifetimes on Windows.
        if (netutils_init()) throw std::runtime_error("network initialization failed");
        struct Network { ~Network() { netutils_cleanup(); } } network;
        ntp_global_init();
        std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
#ifndef _WIN32
        std::signal(SIGPIPE, SIG_IGN);
#endif
        Receiver receiver(options);
        receiver.run();
        return 0;
    } catch (const std::exception& e) { log(std::string("Error: ") + e.what()); return 1; }
}
