#!/usr/bin/env python3
"""Check advertised options, rejected legacy aliases and argument validation."""
import pathlib
import re
import subprocess
import sys
import tempfile


binary = str(pathlib.Path(sys.argv[1]).resolve())
audio_enabled = len(sys.argv) < 3 or sys.argv[2] == "1"
value_options = {
    "--name", "--host", "--video-port", "--audio-port", "--port", "--fps",
    "--mtu", "--device-id", "--keyfile", "--sdp", "--run-seconds", "--password", "-n",
}
flag_options = {"--h265", "--no-audio", "--no-mdns", "--hold", "--debug", "--help", "--random-password", "-d", "-h"}


def invoke(args):
    return subprocess.run([binary, *args], capture_output=True, text=True, timeout=3)


def reject(args, message):
    result = invoke(args)
    assert result.returncode == 1 and message in result.stderr, (args, result)


help_long = invoke(["--help"])
help_short = invoke(["-h"])
assert help_long.returncode == help_short.returncode == 0
assert help_long.stdout == help_short.stdout
advertised = set(re.findall(r"(?<![\w-])--?[a-z][a-z0-9-]*", help_long.stdout))
assert advertised == value_options | flag_options, advertised

for option in ["-fps", "-h265", "-nh", "-nohold", "--nohold", "-as", "-vrtp", "--unknown"]:
    reject([option], f"unknown option: {option}")
reject(["-fps", "30", "--help"], "unknown option: -fps")
reject(["--help", "--unknown"], "unknown option: --unknown")
for option in value_options:
    reject([option], f"missing value for {option}")

for option, bad_values in {
    "--video-port": ["0", "65535"],
    "--audio-port": ["0", "65535"],
    "--port": ["-1", "65536"],
    "--fps": ["0", "121"],
    "--mtu": ["63", "65508"],
    "--run-seconds": ["0", "86401"],
}.items():
    for value in [*bad_values, "abc", "1x", "99999999999999999999"]:
        reject([option, value], "number out of range:")
reject(["--fps", "--help"], "number out of range:")
for name in ["", "x" * 64, "bad\nname", "bad\rname", "界" * 22]:
    reject(["--name", name], "receiver name must contain 1..63 bytes")
for device_id in ["", "02:55:58:50:4c", "02:55:58:50:4c:gg", "02-55-58-50-4c-59"]:
    reject(["--device-id", device_id], "invalid device ID")
for password in ["", "x" * 65, "bad\npassword", "bad\rpassword", "bad\tpassword", "bad\x7fpassword"]:
    reject(["--password", password], "password must contain 1..64 bytes without control characters")
for args in [["--password", "fixed", "--random-password"], ["--random-password", "--password", "fixed"]]:
    reject(args, "--password and --random-password are mutually exclusive")
reject(["--random-password", "1234"], "unknown option: 1234")
if audio_enabled:
    for port in ["4999", "5000", "5001"]:
        reject(["--audio-port", port], "audio and video RTP/RTCP port pairs overlap")

# An invalid numeric destination stops startup before sockets, pairing keys or SDP
# are created. This checks that otherwise valid options pass the entire parser.
with tempfile.TemporaryDirectory(prefix="uxplay-mini-cli-") as folder:
    valid_cases = [
        ["--name", "CLI test", "--video-port", "5200", "--audio-port", "5202",
         "--port", "0", "--fps", "30", "--h265", "--mtu", "1200",
         "--device-id", "02:55:58:50:4C:59", "--keyfile", str(pathlib.Path(folder) / "key.pem"),
         "--sdp", str(pathlib.Path(folder) / "stream.sdp"), "--no-audio", "--no-mdns",
         "--hold", "--debug", "--run-seconds", "1"],
        ["-n", "Short name", "-d"],
        ["--name", "--help"],
        ["--no-audio", "--audio-port", "5000"],
        ["--name", "界" * 21],
        ["--password", "a"],
        ["--password", "x" * 64],
        ["--password", "spaces and punctuation: !\"\\"],
        ["--random-password"],
    ]
    for option, values in {
        "--video-port": ["1", "65534"], "--audio-port": ["1", "65534"],
        "--port": ["0", "65535"], "--fps": ["1", "120"],
        "--mtu": ["64", "65507"], "--run-seconds": ["1", "86400"],
    }.items():
        valid_cases.extend([[option, value] for value in values])
    for args in valid_cases:
        reject([*args, "--host", "not-a-numeric-address"], "invalid numeric destination address:")
    assert not list(pathlib.Path(folder).iterdir()), "argument checks created output files"

print("CLI help, long options, standard short aliases, rejected legacy aliases and validation passed")
