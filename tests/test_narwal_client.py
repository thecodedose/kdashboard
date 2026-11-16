"""Run appliance-control transactions against a mock LAN robot, never a real one."""
import base64
import hashlib
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import unittest
from test_narwal_probe import read_exact, read_client_frame, frame

BINARY = os.environ['NARWAL_CLIENT_BINARY']
IDENTITY = '0123456789abcdef0123456789abcdef'


def varint(value):
    out = bytearray()
    while value >= 128:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def num(field, value):
    return varint(field << 3) + varint(value)


def msg(field, value):
    return varint(field << 3 | 2) + varint(len(value)) + value


def decode(data):
    pos, fields = 0, []

    def vi():
        nonlocal pos
        result, shift = 0, 0
        while True:
            b = data[pos]
            pos += 1
            result |= (b & 127) << shift
            if not b & 128:
                return result
            shift += 7

    while pos < len(data):
        tag = vi()
        wire = tag & 7
        if wire == 0:
            value = vi()
        elif wire == 2:
            length = vi()
            value = data[pos:pos + length]
            pos += length
        else:
            raise ValueError('unexpected wire type')
        fields.append((tag >> 3, value))
    return fields


class ClientTest(unittest.TestCase):
    def run_robot(self, action='status', state=1, paused=False, no_rooms=False, refused=False, timeout=False, options=None):
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        listener.settimeout(8)
        port = listener.getsockname()[1]
        errors, commands = [], []

        def peer():
            try:
                with listener.accept()[0] as conn:
                    conn.settimeout(7)
                    header = b''
                    while not header.endswith(b'\r\n\r\n'):
                        header += read_exact(conn, 1)
                    key = next(line.split(b':', 1)[1].strip() for line in header.split(b'\r\n')
                               if line.lower().startswith(b'sec-websocket-key:'))
                    proof = base64.b64encode(hashlib.sha1(key + b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
                    conn.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '
                                 + proof + b'\r\n\r\n')

                    def receive():
                        opcode, payload = read_client_frame(conn)
                        self.assertEqual(opcode, 2)
                        topic = payload[4:4 + payload[3]].decode()
                        self.assertTrue(topic.startswith('/CNbforyZWI/' + IDENTITY + '/'))
                        short = '/'.join(topic.split('/')[3:])
                        commands.append(short)
                        return short, payload[4 + payload[3]:]

                    def respond(payload):
                        conn.sendall(frame(b'\x01\x02\x2a\x00' + payload))

                    self.assertEqual(receive()[0], 'status/get_device_base_status')
                    task = num(1, state) + (num(2, 1) if paused else b'')
                    status = b'\x15' + struct.pack('<f', 83) + msg(3, task)
                    respond(num(1, 1) + msg(2, status))
                    if action == 'status' or (action == 'start' and state == 4):
                        self.assertEqual(conn.recv(1), b'')
                        return
                    if action == 'start':
                        self.assertEqual(receive()[0], 'map/get_map')
                        rooms = b'' if no_rooms else msg(12, num(1, 101)) + msg(12, num(1, 203))
                        respond(num(1, 1) + msg(2, num(1, 9) + rooms))
                        if no_rooms or (options and options[6] and options[7] != 9):
                            self.assertEqual(conn.recv(1), b'')
                            return
                    topic, payload = receive()
                    expected = {'start': 'clean/start_clean', 'pause': 'task/pause', 'resume': 'task/resume', 'dock': 'supply/recall'}[action]
                    self.assertEqual(topic, expected)
                    if action == 'start':
                        task = decode(dict(decode(payload))[1])
                        self.assertEqual(dict(task)[1], 9, 'must use the active map ID')
                        mode = options[0] if options else 1
                        self.assertEqual(dict(task)[5], mode)
                        items = [decode(value) for field, value in task if field == 2]
                        expected_rooms = [options[6]] if options and options[6] else [101, 203]
                        self.assertEqual([dict(decode(dict(item)[1]))[2] for item in items], expected_rooms)
                        for item in items:
                            params = dict(decode(dict(item)[2]))
                            self.assertEqual(params[1], {1: 2, 2: 3, 3: 5, 4: 4}[mode])
                            self.assertEqual(params[2], options[1] if options else 2)
                            self.assertEqual(params[4], options[2] if options else 2)
                            self.assertEqual(params[3], options[3] if options else 1)
                            self.assertEqual(params[8], options[5] if options else 1)
                            for tag in {1: [5], 2: [6], 3: [5, 6], 4: [7]}[mode]:
                                self.assertEqual(params[tag], options[4] if options else 1)
                            if mode != 4:
                                self.assertNotIn(7, params)
                    else:
                        self.assertEqual(payload, b'')
                    if not timeout:
                        respond(num(1, 2 if refused else 1))
                    self.assertEqual(conn.recv(1), b'', 'must not retry an appliance command')
            except Exception as exc:
                errors.append(exc)
            finally:
                listener.close()

        worker = threading.Thread(target=peer, daemon=True)
        worker.start()
        with tempfile.TemporaryDirectory() as directory:
            identity = Path(directory) / 'identity.txt'
            identity.write_text(IDENTITY + '\n')
            args = [BINARY, '127.0.0.1', str(port), str(identity), action]
            if options:
                args.append(','.join(map(str, options)))
            result = subprocess.run(args, capture_output=True, text=True, timeout=12)
        worker.join(8)
        self.assertFalse(worker.is_alive())
        if errors:
            raise errors[0]
        self.assertNotIn(IDENTITY, result.stdout + result.stderr)
        return result, commands

    def test_status_is_read_only(self):
        r, commands = self.run_robot()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('battery=83 state=Idle', r.stdout)
        self.assertEqual(commands, ['status/get_device_base_status'])

    def test_start_uses_current_map_and_all_rooms_in_vacuum_mode(self):
        self.assertEqual(self.run_robot('start')[0].returncode, 0)

    def test_pause(self):
        self.assertEqual(self.run_robot('pause', state=4)[0].returncode, 0)

    def test_mop_selected_room_and_custom_options(self):
        self.assertEqual(self.run_robot('start', options=(2, 3, 3, 2, 2, 2, 101, 9))[0].returncode, 0)

    def test_vacuum_then_mop(self):
        self.assertEqual(self.run_robot('start', options=(3, 2, 2, 1, 3, 1, 0, 0))[0].returncode, 0)

    def test_vacuum_and_mop(self):
        self.assertEqual(self.run_robot('start', options=(4, 4, 3, 2, 1, 2, 0, 0))[0].returncode, 0)

    def test_map_change_blocks_a_selected_room_clean(self):
        r, commands = self.run_robot('start', options=(1, 2, 2, 1, 1, 1, 101, 8))
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn('clean/start_clean', commands)

    def test_resume(self):
        self.assertEqual(self.run_robot('resume', state=4, paused=True)[0].returncode, 0)

    def test_dock(self):
        self.assertEqual(self.run_robot('dock')[0].returncode, 0)

    def test_no_second_clean_while_already_cleaning(self):
        self.assertNotEqual(self.run_robot('start', state=4)[0].returncode, 0)

    def test_missing_map_rooms_prevents_start(self):
        self.assertNotEqual(self.run_robot('start', no_rooms=True)[0].returncode, 0)

    def test_refused_action_is_reported_and_not_retried(self):
        r, commands = self.run_robot('dock', refused=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('not ready', r.stderr)
        self.assertEqual(commands.count('supply/recall'), 1)

    def test_uncertain_action_is_not_retried(self):
        r, commands = self.run_robot('dock', timeout=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(commands.count('supply/recall'), 1)


if __name__ == '__main__':
    unittest.main()
