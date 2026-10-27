#!/usr/bin/env python3
"""Exercise the real CLI, vcpkg llhttp and binary-plist control path over TCP."""
import pathlib
import plistlib
import queue
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading


def response(stream):
    status = stream.readline().decode().strip()
    headers = {}
    while True:
        line = stream.readline()
        if line == b"\r\n":
            break
        if not line:
            raise RuntimeError("connection closed before response headers")
        key, value = line.decode().split(":", 1)
        headers[key.lower()] = value.strip()
    length = int(headers.get("content-length", 0))
    body = stream.read(length)
    assert len(body) == length
    return status, headers, body


def run(binary, audio_enabled, options):
    assert set(options) <= {"--h265", "--no-audio"}, options
    h265_enabled = "--h265" in options
    audio_output_enabled = audio_enabled and "--no-audio" not in options
    lines = []
    ports = queue.Queue()
    with tempfile.TemporaryDirectory(prefix="uxplay-mini-test-") as folder:
        sdp = pathlib.Path(folder) / "receiver.sdp"
        args = [binary, *options, "--no-mdns", "--run-seconds", "15", "--sdp", str(sdp)]
        child = subprocess.Popen(
            args,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )

        def read_log():
            for line in child.stderr:
                lines.append(line)
                match = re.search(r"listening on TCP (\d+)", line)
                if match:
                    ports.put(int(match[1]))
            ports.put(None)

        reader = threading.Thread(target=read_log, daemon=True)
        reader.start()
        try:
            port = ports.get(timeout=8)
            assert port, "receiver did not start"
            with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
                stream = sock.makefile("rb")
                # Fragment the request to exercise llhttp's incremental callbacks.
                for part in [b"OPTIONS * RT", b"SP/1.0\r\nCS", b"eq: 1\r\n\r\n"]:
                    sock.sendall(part)
                status, headers, _ = response(stream)
                assert status == "RTSP/1.0 200 OK" and headers["cseq"] == "1", (status, headers)
                assert "SETUP" in headers["public"]
                sock.sendall(b"GET /info RTSP/1.0\r\nCSeq: 2\r\n\r\n")
                status, headers, body = response(stream)
                assert status == "RTSP/1.0 200 OK" and headers["cseq"] == "2"
                info = plistlib.loads(body)
                assert info["name"] == "UxPlay Mini"
                assert info["displays"][0]["maxFPS"] == 60
                assert info["features"] & (1 << 7)  # mirroring
                assert not info["features"] & (1 << 4)  # no HLS
                assert bool(info["features"] & (1 << 42)) == h265_enabled
                assert not info["features"] & (1 << 27)  # no legacy pairing delay, even without a password
                assert "audioFormats" in info
                # Output policy must not change the AirPlay handshake. Removing
                # these bits makes iPhone spin and disconnect after ~59 seconds.
                # This also covers builds with UXPLAY_MINI_WITH_AUDIO=OFF.
                for bit in (9, 18, 19, 20, 21):
                    assert info["features"] & (1 << bit), f"missing audio capability bit {bit}"
                sock.sendall(b"GET_PARAMETER * RTSP/1.0\r\nCSeq: 3\r\nContent-Type: text/parameters\r\nContent-Length: 8\r\n\r\nvolume\r\n")
                status, headers, body = response(stream)
                assert status == "RTSP/1.0 200 OK" and b"volume:" in body
                # New pairing code path must not depend on GStreamer/GLib.
                sock.sendall(b"POST /pair-setup RTSP/1.0\r\nCSeq: 4\r\nContent-Length: 32\r\n\r\n" + bytes(32))
                status, _, body = response(stream)
                assert status == "RTSP/1.0 200 OK" and len(body) == 32
                challenge = b"FPLY\x03\x01\x01\x00" + bytes(8)
                sock.sendall(b"POST /fp-setup RTSP/1.0\r\nCSeq: 5\r\nContent-Length: 16\r\n\r\n" + challenge)
                status, _, body = response(stream)
                assert status == "RTSP/1.0 200 OK" and len(body) == 142 and body.startswith(b"FPLY\x03")
                invalid = bytearray(challenge)
                invalid[14] = 255
                sock.sendall(b"POST /fp-setup RTSP/1.0\r\nCSeq: 6\r\nContent-Length: 16\r\n\r\n" + invalid)
                status, _, _ = response(stream)
                assert status.startswith("RTSP/1.0 400"), status
                handshake = b"FPLY\x03" + bytes(139) + bytes(range(20))
                sock.sendall(b"POST /fp-setup RTSP/1.0\r\nCSeq: 7\r\nContent-Length: 164\r\n\r\n" + handshake)
                status, _, body = response(stream)
                assert status == "RTSP/1.0 200 OK" and len(body) == 32 and body[12:] == bytes(range(20))
                sock.sendall(b"GET /server-info HTTP/1.1\r\nHost: localhost\r\n\r\n")
                status, _, _ = response(stream)
                assert status.startswith("HTTP/1.1 501"), status
                stream.close()
            text = sdp.read_text()
            assert "a=rtpmap:96 H264/90000" in text
            assert ("a=rtpmap:98 H265/90000" in text) == h265_enabled
            expected_payloads = "96 98" if h265_enabled else "96"
            assert f"m=video 5000 RTP/AVP {expected_payloads}" in text.splitlines()
            assert ("L16/44100/2" in text) == audio_output_enabled
        except Exception:
            sys.stderr.write("".join(lines))
            raise
        finally:
            child.send_signal(signal.SIGTERM)
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
                raise RuntimeError("receiver did not shut down cleanly")
            reader.join(timeout=1)
        assert child.returncode == 0, "".join(lines)
        assert any("Audio L16 RTP ->" in line for line in lines) == audio_output_enabled
    print(f"TCP RTSP, stable AirPlay audio capabilities, pairing/FairPlay setup, HLS rejection, SDP and shutdown passed (H265: {h265_enabled}, audio output: {audio_output_enabled})")


run(str(pathlib.Path(sys.argv[1]).resolve()), len(sys.argv) < 3 or sys.argv[2] == "1",
    sys.argv[3:])
