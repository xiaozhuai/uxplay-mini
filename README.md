# uxplay-mini

[English](README.md) | [简体中文](README_CN.md)

A standalone AirPlay → RTP receiver trimmed from the UxPlay source, suitable for
use as a sidecar in other applications. It builds independently without a parent
project. It retains LAN discovery, pairing, the FairPlay handshake, audio/video
decryption, audio retransmission, and NTP synchronization, while omitting
GStreamer/GLib, renderers, playback windows, HLS, reverse HTTP/event handling,
recording, X11, and D-Bus.

Video is neither decoded nor transcoded: Annex B data is packetized directly into
H.264/H.265 RTP. Only H.264 is enabled by default; use `--h265` to explicitly allow
H.265/HEVC. AAC-ELD, AAC-LC, and ALAC audio are decoded to L16 PCM RTP through
FFmpeg, using the same implementation on every platform. Only libavcodec and
libavutil are linked; libavformat, libavdevice, libavfilter, libswresample,
libswscale, and the FFmpeg command-line programs are not used.

## Dependencies

| Dependency | Purpose |
| --- | --- |
| OpenSSL libcrypto ≥ 1.1.1 | Pairing, key exchange, and AES decryption; libssl is not linked |
| libplist ≥ 2.3 | AirPlay binary property lists |
| llhttp | Linked through its vcpkg CMake target; llhttp source is not bundled |
| libavcodec ≥ 59, libavutil ≥ 57 | AAC/ALAC decoding and sample-format conversion |
| PlayFair source | FairPlay protocol implementation bundled with UxPlay |

macOS uses system Bonjour by default. Windows, Linux, and FreeBSD use the minimal
mDNS responder retained in the source tree. The built-in backend can also be
selected on macOS. Unix platforms use system pthreads; Windows uses native Win32
threading and synchronization primitives, so no separate pthread library is
needed.

Builds and automated tests have currently been verified only on macOS arm64.
Platform implementations for Windows, Linux, and FreeBSD are retained but have
not yet been verified by builds or real-device tests on those systems.

## Building

`CMakePresets.json` provides reference build configurations. Set `VCPKG_ROOT` to
your vcpkg installation directory, then run these commands from the project root:

```sh
cmake --preset arm64-macos-release
cmake --build build/binary/arm64-macos-release
ctest --test-dir build/binary/arm64-macos-release --output-on-failure
```

For Debug builds, use `arm64-macos-debug`. Corresponding desktop presets are
provided for Windows and Linux. Mobile and WebAssembly presets are retained only
as references and do not imply support for those platforms.

To build without a preset:

```sh
cmake -S . -B build/local -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build/local
```

The same approach can be used on FreeBSD, with `VCPKG_TARGET_TRIPLET` set as needed
for your installed dependencies. Before building on FreeBSD, make sure your
FFmpeg port supports that platform and that libavcodec/libavutil are available.
The project does not automatically modify or install platform dependencies.

Configuration options:

| Option | Default | Purpose |
| --- | --- | --- |
| `UXPLAY_MINI_WITH_AUDIO` | `ON` | Set to `OFF` to remove FFmpeg linkage entirely and build a video-only receiver |
| `UXPLAY_MINI_INTERNAL_MDNS` | `OFF` | Use built-in mDNS on macOS; other platforms always use the built-in backend |
| `UXPLAY_MINI_SANITIZERS` | `OFF` | Enable ASan and UBSan for project sources |
| `BUILD_TESTING` | `ON` | Build unit and integration tests; Python 3 is needed only for integration tests |

## Usage

Use `--no-audio` for video-only output: audio packets are accepted and discarded
without decoding or outgoing audio RTP/RTCP. AirPlay audio capability negotiation
is intentionally preserved; removing those capabilities can leave iPhone stuck
connecting until it disconnects after about a minute. The same behavior applies
to builds with `UXPLAY_MINI_WITH_AUDIO=OFF`, which do not link FFmpeg at all.

```sh
./build/binary/arm64-macos-release/uxplay-mini \
  -n "UxPlay Mini" --host 127.0.0.1 \
  --video-port 5000 --audio-port 5002 --sdp build/uxplay-mini.sdp
```

On an iPhone connected to the same LAN, open **Screen Mirroring** and select
`UxPlay Mini`. The program does not open a playback window; it sends media to the
destination over UDP. Exit with Ctrl-C or SIGTERM. The receiving application
should listen on the configured ports before streaming starts. Run `--help` for
all options.

Legacy pairing advertisement is disabled in all password modes, including without
a password, to reduce connection delay. The FairPlay handshake and media encryption
remain enabled.

### Fixed AirPlay password

Add `--password` to require a password when an iPhone connects:

```sh
./build/binary/arm64-macos-release/uxplay-mini --name "Private receiver" --password "example-pass"
```

Enter the same password on the iPhone when prompted. It remains unchanged across
connections and process restarts when the same argument is supplied. Omit both
password options to disable authentication. Passwords must contain 1–64 bytes and
no control characters; an empty password is rejected.
The password is not printed in mini's logs, but command-line arguments can be
visible in operating-system process listings.

This authenticates incoming AirPlay sessions. Outgoing RTP/RTCP and audio/video
formats are unchanged. The desktop app must explicitly pass this argument to use
password authentication; its current launcher does not do so.

### Random AirPlay password

Use `--random-password` to generate a temporary four-digit password when a client
starts authentication:

```sh
./build/binary/arm64-macos-release/uxplay-mini --name "Private receiver" --random-password
```

Select the receiver on iPhone, then enter the code printed in the terminal as
`AirPlay PIN: 0123`. Keep any leading zeroes. The code remains valid during the
same authentication attempt, including repeated challenges. After successful
authentication, a later session generates a new code. Switching clients or
reaching three failed authentication attempts also causes a new code to be
generated and displayed on the next attempt. A newly generated code can happen
to have the same digits as an earlier one.

`--password` and `--random-password` are mutually exclusive. Omit both to disable
password authentication. Both modes use SETUP Digest authentication with PIN/SRP
pairing advertisement disabled. Random passwords are displayed in logs so the
user can enter them; fixed passwords are not. The desktop launcher does not yet
enable random passwords or display their codes.

All multi-character options use two hyphens, with values separated by spaces.
The only single-letter aliases are `-h` (help), `-n` (name), and `-d` (debug).
Legacy UxPlay command-line aliases are not supported. Unknown options, missing
values, and out-of-range numbers are reported as errors rather than silently
ignored.

By default, the receiver does not advertise HEVC support and rejects H.265 video.
Add `--h265` to enable the corresponding AirPlay capability bit and include
H265/90000 with payload type 98 in the SDP. H.264 remains available when this
option is enabled; the sender chooses the codec, and no transcoding is forced.
Do not enable this option when integrating with an H.264-only client.

| Media | Default RTP port | PT | Encoding and clock |
| --- | --- | --- | --- |
| H.264 video | 5000 | 96 | H264/90000; RFC 6184 single NAL / FU-A |
| H.265 video (requires `--h265`) | 5000 | 98 | H265/90000; RFC 7798 single NAL / FU |
| Audio | 5002 | 97 | L16/44100/2; signed 16-bit big-endian, interleaved stereo |

The default maximum RTP UDP datagram size is 1200 bytes; adjust it with `--mtu`.
The last packet of each video access unit has the marker bit set. L16 packets
contain complete stereo sample frames, and their timestamps advance by sample
count. Audio preserves the client's original RTP sample clock rather than using
UDP arrival times to generate sample timestamps. When NTP is not yet available,
a local time anchor is established once and advanced by sample count, preventing
arrival jitter from becoming gaps of silence.

RTCP is sent to each RTP port + 1 and includes Sender Reports and a shared CNAME
to map both media clocks onto the same NTP timeline. RTCP feedback and downstream
packet-loss retransmission are not implemented.

Use `--device-id` to specify a stable, unique receiver identifier in MAC-address
format. Multiple instances should have distinct names, device IDs, and output
ports. `--keyfile` persists a randomly generated pairing key; without it, the
upstream behavior of deriving the key from the device ID is retained. New clients
can take over an existing connection by default; `--hold` prevents takeover.

## Integrating an RTP Client

Set the destination address and ports with `--host`, `--video-port`, and
`--audio-port`. Handle incoming RTP using the payload types and encodings listed
above. No GStreamer runtime is required. This project provides only the receiver
and forwarder, not a player or application-specific integration code.

For Electron audio playback, listen for audio RTP, depacketize L16, read samples
as big-endian int16 values, convert them to floating-point PCM, and feed them to
an AudioWorklet. RTP packets cannot be passed directly to HTML audio or
WebCodecs. Native Electron support for AAC-ELD or ALAC decoding is not required.
The player must provide appropriate buffering and clock synchronization.

## Validation Scope

Automated tests cover H.264/H.265 fragmentation and reassembly, marker bits,
sequence-number wraparound, RTP/RTCP clock mapping, CLI help, option naming,
rejection of legacy aliases, missing values, and value ranges. They also cover
L16 byte order, audio without NTP, clock jitter, audio sample-timestamp wraparound,
AAC-ELD/AAC-LC initialization, actual ALAC decoding, UDP transmission and reception,
incremental RTSP parsing, binary property lists, the pairing setup endpoint,
HLS rejection, default and opt-in H.265 capability bits and SDP, and process exit.
Password tests cover Digest challenges, correct and incorrect passwords, malformed
and reordered authorization fields, stale nonces, qop, unauthenticated stream setup,
and invalid PIN-pairing and SETUP key lengths. DNS-SD tests also verify the password
advertisement. These protocol tests do not complete a real iPhone pairing session.
Random-password tests also cover code generation and display, stable challenges,
wrong and correct codes, renewal after success, retry-limit rotation and client changes.
Explicit `--no-audio` tests check that AirPlay audio capabilities remain available
in `/info` and DNS-SD TXT records while outgoing audio is omitted from SDP and logs.
On macOS, an independent system `dns-sd` client also checks AirPlay service ports,
TXT records, and the HEVC capability bit.

These tests do not replace full iPhone acceptance testing. The complete AirPlay
handshake, real mirrored media, long-running audio/video synchronization, and
other desktop platforms still require real-device validation.

For source provenance, trimming notes, and licensing, see
[`UPSTREAM.md`](UPSTREAM.md) and [`LICENSE`](LICENSE). The overall project is
GPL-3.0-or-later; license notices in the original files are preserved.
