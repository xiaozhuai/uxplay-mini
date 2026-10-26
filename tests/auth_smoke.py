#!/usr/bin/env python3
"""Check fixed-password Digest authentication and upstream input-length fixes."""
import hashlib
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

PASSWORD = 'mini-test: fixed password'


def response(stream):
    status = stream.readline().decode().strip()
    headers = {}
    while True:
        line = stream.readline()
        if line == b'\r\n':
            break
        if not line:
            raise RuntimeError('connection closed before response headers')
        key, value = line.decode().split(':', 1)
        headers[key.lower()] = value.strip()
    length = int(headers.get('content-length', 0))
    body = stream.read(length)
    assert len(body) == length
    return int(status.split()[1]), headers, body


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=3)
        self.stream = self.sock.makefile('rb')
        self.sequence = 0

    def request(self, payload=None, authorization=None, method='SETUP', uri='/stream'):
        self.sequence += 1
        body = plistlib.dumps(payload, fmt=plistlib.FMT_BINARY) if payload is not None else b''
        headers = [f'{method} {uri} RTSP/1.0', f'CSeq: {self.sequence}', f'Content-Length: {len(body)}']
        if payload is not None:
            headers.append('Content-Type: application/x-apple-binary-plist')
        if authorization is not None:
            headers.append('Authorization: ' + authorization)
        self.sock.sendall(('\r\n'.join(headers) + '\r\n\r\n').encode() + body)
        return response(self.stream)

    def close(self):
        self.stream.close()
        self.sock.close()


def digest(nonce, password=PASSWORD, uri='/stream', realm='raop', qop=False):
    def md5(text):
        return hashlib.md5(text.encode()).hexdigest()
    first = md5('AirPlay:' + realm + ':' + password)
    second = md5('SETUP:' + uri)
    middle = nonce + (':00000001:auth-test:auth' if qop else '')
    proof = md5(first + ':' + middle + ':' + second)
    # Deliberately reorder fields to exercise parsing independently of order.
    header = f'Digest response="{proof}", uri="{uri}", username="AirPlay", realm="{realm}", nonce="{nonce}"'
    if qop:
        header += ', qop=auth, nc=00000001, cnonce="auth-test", algorithm=MD5'
    return header


def challenge(reply):
    status, headers, _ = reply
    assert status == 401, reply
    match = re.fullmatch(r'Digest realm="raop", nonce="([0-9a-f]{32})"', headers['www-authenticate'])
    assert match, headers
    return match[1]


def run(binary):
    lines, ports = [], queue.Queue()
    with tempfile.TemporaryDirectory(prefix='uxplay-mini-auth-') as folder:
        child = subprocess.Popen(
            [binary, '--password', PASSWORD, '--no-audio', '--no-mdns', '--debug',
             '--keyfile', str(pathlib.Path(folder) / 'pairing.key'), '--run-seconds', '20'],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )

        def read_log():
            for line in child.stderr:
                lines.append(line)
                match = re.search(r'listening on TCP (\d+)', line)
                if match:
                    ports.put(int(match[1]))
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
            client = connect()
            assert client.request(method='OPTIONS', uri='*')[0] == 200
            status, _, body = client.request(method='GET', uri='/info')
            assert status == 200
            assert not plistlib.loads(body)['features'] & (1 << 27), 'fixed-password mode must not advertise PIN pairing'
            # Bad key lengths allow authentication to be tested without creating
            # a fake FairPlay session: only authenticated requests reach HTTP 400.
            setup = {'deviceID': '02:55:58:50:4c:59', 'eiv': b'x', 'ekey': b'x'}
            nonce = challenge(client.request(setup))
            malformed = [
                lambda n: 'Basic dGVzdA==', lambda n: 'Digest',
                lambda n: 'Digest username="unterminated',
                lambda n: 'Digest nonce="' + n + '"',
                lambda n: digest(n, password='wrong'), lambda n: digest('0' * 32),
                lambda n: digest(n, uri='/other'), lambda n: digest(n, realm='other'),
                lambda n: digest(n) + ', nonce="duplicate"',
                lambda n: digest(n) + ', qop=auth',
                lambda n: digest(n) + ', algorithm=SHA-256',
            ]
            for make_header in malformed:
                nonce = challenge(client.request(setup, make_header(nonce)))
            # Successful Digest, including quoted values with spaces in password.
            accepted = digest(nonce)
            assert client.request(setup, accepted)[0] == 400
            assert client.request(setup)[0] == 400  # same authenticated connection
            # Close each RTSP connection before opening another: nohold mode
            # intentionally replaces the preceding RAOP connection.
            client.close()
            clients.remove(client)
            unauthenticated = connect()
            assert unauthenticated.request({'streams': [{'type': 110}]})[0] == 401
            unauthenticated.close()
            clients.remove(unauthenticated)
            # Replaying the consumed authorization on a new connection must fail.
            client = connect()
            nonce = challenge(client.request(setup, accepted))
            # qop=auth is also accepted, even with reordered fields.
            assert client.request(setup, digest(nonce, qop=True))[0] == 400
            for iv_size, key_size in [(0, 72), (15, 72), (17, 72), (16, 0), (16, 71), (16, 73)]:
                invalid = {**setup, 'eiv': bytes(iv_size), 'ekey': bytes(key_size)}
                assert client.request(invalid)[0] == 400
            # Invalid SRP proof/key/tag lengths must reject without overreading.
            for size in [0, 19, 21, 64]:
                assert client.request({'pk': b'x', 'proof': bytes(size)}, method='POST', uri='/pair-setup-pin')[0] == 470
            for key_size, tag_size in [(0, 16), (31, 16), (33, 16), (32, 0), (32, 15), (32, 17)]:
                assert client.request({'epk': bytes(key_size), 'authTag': bytes(tag_size)}, method='POST', uri='/pair-setup-pin')[0] == 470
            assert client.request(method='OPTIONS', uri='*')[0] == 200
        except Exception:
            sys.stderr.write(''.join(lines))
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
        assert child.returncode == 0, ''.join(lines)
        assert PASSWORD not in ''.join(lines), 'password leaked in receiver logs'
        assert not any('ERROR: AddressSanitizer' in line or 'runtime error:' in line for line in lines), ''.join(lines)
    print('Fixed password, wrong/malformed/replayed Digest, qop, SETUP bypass and pairing/key lengths passed')


if __name__ == '__main__':
    run(str(pathlib.Path(sys.argv[1]).resolve()))
