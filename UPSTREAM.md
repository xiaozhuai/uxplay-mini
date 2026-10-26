# Upstream source

Protocol core imported from https://github.com/FDH2/UxPlay
at commit `95a0e49561a52e4b52ef59c8e17cdd701509d35a` (2026-10-23).
Only `lib/` protocol sources and the bundled PlayFair
implementation are retained. Copyright and individual license notices are preserved.
The combined project is distributed under GPL-3.0-or-later (see LICENSE).

Local changes:

- Added opt-in fixed AirPlay access passwords through `--password`, including
  password-required discovery records on both mDNS backends. Like upstream `-pw`,
  this mode clears feature bit 27 so iPhone uses SETUP Digest authentication
  instead of requesting a random SRP pairing PIN. No random-PIN CLI.
- Removed HLS HTTP handlers, playlist proxy, media store and playback callbacks.
- Removed the unused reverse-HTTP/Event channel and associated request parser code.
- Replaced the upstream executable and all GStreamer renderers with a small CLI,
  direct UDP RTP packetizers and an audio-only decoder.
- System Bonjour on macOS; bundled mDNS on other systems (optional on macOS).
- The CLI uses double-hyphen long options, with only `-h`, `-n` and `-d` short
  aliases. Upstream CLI compatibility aliases are not retained.
- H.265/HEVC is opt-in with `--h265` and disabled by default.
  The switch controls AirPlay feature bit 42, codec acceptance and SDP;
  enabled HEVC video uses RTP payload type 98 without transcoding.
- External llhttp is provided by vcpkg instead of bundled source.
- Kept OpenSSL libcrypto and libplist for pairing, decryption and binary plists.
- All platforms use libavcodec and libavutil for audio decoding. Video does not
  pass through a decoder, parser library or encoder.
- Video-only mode retains AirPlay audio negotiation capabilities for iPhone
  compatibility; it discards incoming audio without decoding or forwarding it.
- Replaced pthread usage on Windows with Win32 threads, mutexes and condition
  variables. Unix platforms continue to use system pthreads.
- Use IPPROTO_TCP directly for portable TCP keepalive socket-option levels.
- Made protocol byte reads/writes alignment-safe and guarded partial-init cleanup.
- Validated the FairPlay challenge mode before indexing the response table.

CMakePresets.json provides reference build configurations.
Its presence is not a claim that mobile/WebAssembly targets are supported.
