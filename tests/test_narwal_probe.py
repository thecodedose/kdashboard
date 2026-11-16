"""Exercise the native diagnostic against a real local WebSocket test peer."""
import base64
import hashlib
import os
import socket
import struct
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

BINARY = os.environ['NARWAL_PROBE_BINARY']
GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'


def read_exact(conn, length):
    data = b''
    while len(data) < length:
        more = conn.recv(length - len(data))
        if not more:
            raise EOFError('client disconnected')
        data += more
    return data


def read_client_frame(conn):
    a, b = read_exact(conn, 2)
    assert b & 128, 'client must mask all frames'
    length = b & 127
    if length == 126:
        length = struct.unpack('!H', read_exact(conn, 2))[0]
    mask = read_exact(conn, 4)
    payload = read_exact(conn, length)
    return a & 15, bytes(v ^ mask[i % 4] for i, v in enumerate(payload))


def frame(payload, opcode=2, final=True):
    a = opcode | (128 if final else 0)
    if len(payload) < 126:
        return bytes([a, len(payload)]) + payload
    return bytes([a, 126]) + struct.pack('!H', len(payload)) + payload


def status(payload=b'\x15' + struct.pack('<f', 87)):
    topic = b'/CNbforyZWI/0123456789abcdef0123456789abcdef/status/robot_base_status'
    return bytes([1, len(topic) + 2, 0x22, len(topic)]) + topic + payload


class ProbeTest(unittest.TestCase):
    def run_peer(self, behavior, identity_file=None):
        errors = []
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        listener.settimeout(5)
        port = listener.getsockname()[1]

        def peer():
            try:
                with listener.accept()[0] as conn:
                    conn.settimeout(5)
                    headers = b''
                    while not headers.endswith(b'\r\n\r\n'):
                        headers += read_exact(conn, 1)
                    key = next(line.split(b':', 1)[1].strip() for line in headers.split(b'\r\n')
                               if line.lower().startswith(b'sec-websocket-key:'))
                    self.assertEqual(len(base64.b64decode(key)), 16)
                    proof = base64.b64encode(hashlib.sha1(key + GUID.encode()).digest())
                    if behavior == 'bad_accept':
                        proof = b'incorrect'
                    conn.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n'
                                 b'Connection: Upgrade\r\nSec-WebSocket-Accept: ' + proof + b'\r\n\r\n')
                    if behavior == 'bad_accept':
                        return
                    opcode, request = read_client_frame(conn)
                    self.assertEqual(opcode, 2)
                    identity = '0123456789abcdef0123456789abcdef' if behavior == 'addressed' else ''
                    self.assertEqual(request[4:].decode(), '/CNbforyZWI/' + identity + '/common/get_device_info')
                    if behavior == 'timeout':
                        while conn.recv(1024):
                            pass
                    elif behavior == 'oversize':
                        conn.sendall(b'\x82\x7f' + struct.pack('!Q', 65537))
                    elif behavior == 'bad_proto':
                        conn.sendall(frame(status(b'\x15\x01')))
                    elif behavior == 'nan':
                        conn.sendall(frame(status(b'\x15' + struct.pack('<f', float('nan')))))
                    elif behavior == 'fragmented':
                        data = status()
                        conn.sendall(frame(data[:20], final=False) + frame(b'ping', opcode=9)
                                     + frame(data[20:], opcode=0))
                        op, pong = read_client_frame(conn)
                        self.assertEqual((op, pong), (10, b'ping'))
                    elif behavior in ('identity_response', 'addressed'):
                        identity = b'0123456789abcdef0123456789abcdef'
                        if behavior == 'identity_response':
                            info = b'\x0a\x0aCNbforyZWI\x12\x20' + identity
                            conn.sendall(frame(b'\x01\x02\x2a\x00' + info))
                        op, query = read_client_frame(conn)
                        self.assertEqual(query[4:], b'/CNbforyZWI/' + identity + b'/status/get_device_base_status')
                        battery = b'\x15' + struct.pack('<f', 87)
                        response = b'\x08\x01\x12' + bytes([len(battery)]) + battery
                        conn.sendall(frame(b'\x01\x02\x2a\x00' + response))
                    else:
                        conn.sendall(frame(status()))
            except Exception as exc:
                errors.append(exc)
            finally:
                listener.close()

        worker = threading.Thread(target=peer, daemon=True)
        worker.start()
        args = [BINARY, '127.0.0.1', '--port', str(port), '--timeout', '2']
        if identity_file is not None:
            args += ['--identity-file', str(identity_file)]
        result = subprocess.run(args,
                                capture_output=True, text=True, timeout=6)
        worker.join(6)
        self.assertFalse(worker.is_alive(), 'test peer did not finish')
        if errors:
            raise errors[0]
        return result

    def test_live_status(self):
        r = self.run_peer('status')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('Battery: 87%', r.stdout)
        self.assertNotIn('0123456789abcdef', r.stdout)

    def test_fragmented_status_with_ping(self):
        r = self.run_peer('fragmented')
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_rejects_wrong_upgrade_proof(self):
        r = self.run_peer('bad_accept')
        self.assertEqual(r.returncode, 1)
        self.assertIn('invalid WebSocket accept proof', r.stderr)

    def test_websocket_only_is_not_reported_as_live_status(self):
        r = self.run_peer('timeout')
        self.assertEqual(r.returncode, 2)
        self.assertNotIn('RESULT: live status', r.stdout)

    def test_rejects_oversized_frame(self):
        r = self.run_peer('oversize')
        self.assertEqual(r.returncode, 2)
        self.assertIn('frame too large', r.stderr)

    def test_rejects_truncated_status(self):
        r = self.run_peer('bad_proto')
        self.assertEqual(r.returncode, 2)
        self.assertIn('truncated protobuf field', r.stderr)

    def test_rejects_invalid_battery(self):
        r = self.run_peer('nan')
        self.assertEqual(r.returncode, 2)
        self.assertIn('invalid battery reading', r.stderr)

    def test_discovers_and_saves_identity_then_reads_status_response(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / 'identity.txt'
            r = self.run_peer('identity_response', cache)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(cache.read_text(), '0123456789abcdef0123456789abcdef\n')
            self.assertIn('Battery: 87%', r.stdout)
            self.assertNotIn('0123456789abcdef', r.stdout + r.stderr)

    def test_cached_identity_reads_status_without_any_broadcast(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / 'identity.txt'
            cache.write_text('0123456789abcdef0123456789abcdef\n')
            r = self.run_peer('addressed', cache)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn('Battery: 87%', r.stdout)
            self.assertNotIn('0123456789abcdef', r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
