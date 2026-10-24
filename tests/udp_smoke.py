#!/usr/bin/env python3
"""Verify actual RTP/RTCP UDP sockets and receive a complete video/audio fixture."""
import contextlib
import socket
import struct
import subprocess
import sys


def pair(stack):
    for _ in range(100):
        rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rtp.bind(("127.0.0.1", 0))
        port = rtp.getsockname()[1]
        rtcp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            rtcp.bind(("127.0.0.1", port + 1))
        except (OSError, OverflowError):
            rtp.close()
            rtcp.close()
            continue
        stack.enter_context(rtp)
        stack.enter_context(rtcp)
        rtp.settimeout(3)
        rtcp.settimeout(3)
        return rtp, rtcp, port
    raise RuntimeError("could not reserve RTP/RTCP test ports")


with contextlib.ExitStack() as stack:
    video, video_control, vp = pair(stack)
    audio, audio_control, ap = pair(stack)
    subprocess.run([sys.argv[1], str(vp), str(ap)], check=True, timeout=8)
    fragments = [video.recv(65536), video.recv(65536)]
    assert all(len(p) <= 1200 and p[1] & 0x7f == 96 for p in fragments)
    rebuilt = bytes([(fragments[0][12] & 0xe0) | (fragments[0][13] & 0x1f)])
    rebuilt += b"".join(p[14:] for p in fragments)
    assert rebuilt == b"\x65" + bytes([0xab]) * 2000
    assert not fragments[0][1] & 0x80 and fragments[-1][1] & 0x80
    assert video_control.recv(65536)[1] == 200
    packets = [audio.recv(65536) for _ in range(4)]
    assert all(len(p) <= 1200 and p[1] & 0x7f == 97 for p in packets)
    received = b"".join(p[12:] for p in packets)
    expected = struct.pack(">2000h", *range(-1000, 1000))
    assert received == expected
    assert audio_control.recv(65536)[1] == 200
print("Real UDP H264 fragmentation, L16 sample transport and RTCP passed")
