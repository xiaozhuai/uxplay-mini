#!/usr/bin/env python3
"""Exercise generated-password lifetime, retries and client changes over RTSP."""
import pathlib
import plistlib
import queue
import re
import signal
import subprocess
import sys
import tempfile
import threading

sys.dont_write_bytecode = True
from auth_smoke import Client, challenge, digest


def run(binary):
    lines, ports, pins = [], queue.Queue(), queue.Queue()
    with tempfile.TemporaryDirectory(prefix='uxplay-mini-random-auth-') as folder:
        child = subprocess.Popen(
            [binary, '--random-password', '--no-audio', '--no-mdns',
             '--keyfile', str(pathlib.Path(folder) / 'pairing.key'), '--run-seconds', '25'],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )

        def read_log():
            for line in child.stderr:
                lines.append(line)
                match = re.search(r'listening on TCP (\d+)', line)
                if match:
                    ports.put(int(match[1]))
                match = re.fullmatch(r'AirPlay PIN: (\d{4})\n', line)
                if match:
                    pins.put(match[1])
            ports.put(None)

        reader = threading.Thread(target=read_log, daemon=True)
        reader.start()
        clients = []
        try:
            port = ports.get(timeout=8)
            assert port, 'receiver did not start'

            def connect():
                client = Client(port)
                clients.append(client)
                return client

            def close(client):
                client.close()
                clients.remove(client)

            def no_new_pin():
                try:
                    extra = pins.get(timeout=0.05)
                except queue.Empty:
                    return
                raise AssertionError('unexpected PIN generation: ' + extra)

            setup = {'deviceID': '02:55:58:50:4c:59', 'eiv': b'x', 'ekey': b'x'}
            client = connect()
            status, _, body = client.request(method='GET', uri='/info')
            assert status == 200 and not plistlib.loads(body)['features'] & (1 << 27)
            # Missing identity must fail cleanly before associating a generated PIN.
            assert client.request({'eiv': b'x', 'ekey': b'x'})[0] == 400
            assert client.request({**setup, 'deviceID': ''})[0] == 400
            no_new_pin()
            nonce = challenge(client.request(setup))
            pin = pins.get(timeout=3)
            # Repeated challenges are not failed password attempts and keep the code.
            for _ in range(4):
                nonce = challenge(client.request(setup))
            no_new_pin()
            wrong = f'{(int(pin) + 1) % 10000:04d}'
            nonce = challenge(client.request(setup, digest(nonce, password=wrong)))
            no_new_pin()
            assert client.request(setup, digest(nonce, password=pin))[0] == 400
            assert client.request(setup)[0] == 400  # authenticated connection stays open
            no_new_pin()
            close(client)

            # A new session must generate and display another code after success.
            client = connect()
            nonce = challenge(client.request(setup))
            pin = pins.get(timeout=3)
            for _ in range(3):
                nonce = challenge(client.request(setup, 'Digest'))
            no_new_pin()
            # The next attempt rotates the code, even if it includes Authorization.
            nonce = challenge(client.request(setup, 'Digest'))
            pin = pins.get(timeout=3)
            assert client.request(setup, digest(nonce, password=pin))[0] == 400
            close(client)

            # A different client cannot reuse another client's pending code.
            client = connect()
            nonce = challenge(client.request(setup))
            pins.get(timeout=3)
            changed = {**setup, 'deviceID': '02:55:58:50:4c:60'}
            nonce = challenge(client.request(changed, 'Digest'))
            pin = pins.get(timeout=3)
            assert client.request(changed, digest(nonce, password=pin))[0] == 400
        except Exception:
            sys.stderr.write(''.join(lines[-100:]))
            raise
        finally:
            for client in clients:
                client.close()
            child.send_signal(signal.SIGTERM)
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
                raise RuntimeError('receiver did not shut down cleanly')
            reader.join(timeout=1)
        assert child.returncode == 0, ''.join(lines[-100:])
        assert not any('ERROR: AddressSanitizer' in line or 'runtime error:' in line for line in lines), ''.join(lines)
        assert sum(line.startswith('AirPlay PIN: ') for line in lines) == 5
        assert sum('Client authentication success' in line for line in lines) == 3
        assert not any('PAIR-PIN-START' in line or 'pair-setup-pin' in line for line in lines)
    print('Random PIN generation, stable retries, wrong/correct passwords, rotation and client changes passed')


if __name__ == '__main__':
    run(str(pathlib.Path(sys.argv[1]).resolve()))
