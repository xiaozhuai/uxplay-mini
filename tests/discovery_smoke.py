#!/usr/bin/env python3
"""Resolve an advertised AirPlay service using macOS's independent DNS-SD client."""
import queue
import re
import signal
import subprocess
import sys
import threading


h265_enabled = len(sys.argv) > 3
name = "UxPlay Mini discovery H265 test" if h265_enabled else "UxPlay Mini discovery test"
ready = queue.Queue()
logs = []
args = [sys.argv[1], "--name", name, "--run-seconds", "12"]
if h265_enabled:
    args.append(sys.argv[3])
server = subprocess.Popen(
    args,
    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
)


def read_logs():
    for line in server.stderr:
        logs.append(line)
        match = re.search(r"listening on TCP (\d+)", line)
        if match:
            ready.put(int(match[1]))
    ready.put(None)


thread = threading.Thread(target=read_logs, daemon=True)
thread.start()
client = None
try:
    port = ready.get(timeout=8)
    assert port, "service did not start"
    client = subprocess.Popen(
        [sys.argv[2], "-L", name, "_airplay._tcp", "local."],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    try:
        output, _ = client.communicate(timeout=3)
    except subprocess.TimeoutExpired:
        client.send_signal(signal.SIGINT)
        output, _ = client.communicate(timeout=2)
    assert f":{port}" in output and "deviceid=" in output and "features=" in output, output
    match = re.search(r"features=(0x[0-9a-fA-F]+),(0x[0-9a-fA-F]+)", output)
    assert match, output
    features = int(match[1], 16) | (int(match[2], 16) << 32)
    assert bool(features & (1 << 42)) == h265_enabled, output
finally:
    if client and client.poll() is None:
        client.kill()
        client.wait()
    server.send_signal(signal.SIGTERM)
    server.wait(timeout=5)
    thread.join(timeout=1)
assert server.returncode == 0, "".join(logs)
print(f"Independent DNS-SD client resolved AirPlay port and TXT records (H265: {h265_enabled})")
