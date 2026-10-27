# Upstream source

Protocol core imported from https://github.com/FDH2/UxPlay
at commit `95a0e49561a52e4b52ef59c8e17cdd701509d35a` (2026-10-23).
Only `lib/` protocol sources and the bundled PlayFair
implementation are retained. Copyright and individual license notices are preserved.
The combined project is distributed under GPL-3.0-or-later (see LICENSE).

Additional upstream commits selectively backported on 2026-10-26:

- [`60e643008adb7bdd8eb324ada63dc766af041841`](https://github.com/FDH2/UxPlay/commit/60e643008adb7bdd8eb324ada63dc766af041841):
  imported the crypto, SRP, pairing and SETUP fixes from `lib/`. The upstream UI
  and removed reverse-HTTP parser changes are excluded. Corrected the new invalid
  key-length diagnostic and cleaned up plists on that error path; SRP temporary
  buffers are also released when verifier creation fails.
- [`bced29377447917303df5d8002873ea483a54612`](https://github.com/FDH2/UxPlay/commit/bced29377447917303df5d8002873ea483a54612):
  imported the nonce-generation correction and password-challenge cleanup.

The base import above remains unchanged; this is not a full upstream revision update.

Local changes:

- Added mutually exclusive fixed (`--password`) and generated (`--random-password`)
  AirPlay passwords, including password-required records on both mDNS backends.
  Like upstream `-pw`, both modes use SETUP Digest authentication instead of
  requesting a random SRP pairing PIN.
- Clear feature bit 27 in all password modes, including without a password, to
  avoid the legacy-pairing connection delay. FairPlay and media encryption remain enabled.
- Random passwords use the retained upstream generator and PIN display callback.
  Failed authentication proofs, rather than code displays, consume retry attempts;
  regenerated codes are always displayed. Missing client identities and random
  generation/allocation failures reject the request instead of using a fallback code.
- Hardened Digest parsing, binding credentials to the current nonce, realm and
  request URI; unauthenticated streams-only SETUP cannot skip password validation.
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
