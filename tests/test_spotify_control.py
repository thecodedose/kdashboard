"""Linux integration checks for the prototype's fixed-command control service."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux service")
class ControlService(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="kindle-spotify-control-")
        cls.base = Path(cls.temp.name)
        (cls.base / "private").mkdir()
        (cls.base / "private/control.token").write_text("test-token-at-least-24-characters")
        source = Path(__file__).resolve().parents[1] / "kindle/native/src/spotify_control.c"
        cls.binary = cls.base / "control"
        subprocess.run(["gcc", "-O2", "-Wall", "-Wextra", "-Werror",
                        '-DBASE="' + str(cls.base) + '"', '-DRUN="' + str(cls.base) + '"',
                        str(source), "-o", str(cls.binary)], check=True)
        env = dict(os.environ)
        env.pop("SPOTIFY_DEBUG_LAN", None)
        cls.process = subprocess.Popen([str(cls.binary), "--server"], env=env)
        for _ in range(50):
            try:
                with socket.create_connection(("127.0.0.1", 24880), timeout=0.1):
                    return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("Test service did not start")

    @classmethod
    def tearDownClass(cls):
        cls.process.terminate()
        cls.process.wait(timeout=3)
        cls.temp.cleanup()

    def request(self, path, method="GET", token="test-token-at-least-24-characters", origin=None):
        headers = {"X-Dashboard-Toggle-Token": token}
        if origin:
            headers["Origin"] = origin
        req = urllib.request.Request("http://127.0.0.1:24880" + path, headers=headers, method=method)
        try:
            response = urllib.request.urlopen(req, timeout=6)
        except urllib.error.HTTPError as error:
            response = error
        return response.status, response.read()

    def test_authorization_and_browser_origin(self):
        self.assertEqual(self.request("/health", token="wrong")[0], 401)
        self.assertEqual(self.request("/health", origin="https://untrusted.invalid")[0], 401)
        status, body = self.request("/health")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["player"], "Kindle Spotify")

    def test_only_valid_fixed_commands(self):
        for path in ["/volume/101", "/volume/30;id", "/audio-volume/101", "/audio-volume/30;id", "/playlist/../../etc/passwd", "/sink-mode/other", "/sink-mode/clocked;id", "/native-tone/other", "/native-tone/clocked;id"]:
            self.assertEqual(self.request(path, "POST")[0], 400)
        self.assertEqual(self.request("/unknown", "POST")[0], 404)
        self.assertEqual(self.request("/restart")[0], 404)
        self.assertEqual(self.request("/restart", "POST")[0], 202)
        self.assertTrue((self.base / "restart").exists())

    def test_sink_mode_is_limited_to_fixed_choices(self):
        for mode in ["clocked", "unclocked"]:
            self.assertEqual(self.request("/sink-mode/" + mode, "POST")[0], 202)
            self.assertEqual((self.base / "sink-mode").read_text(), mode)
        self.assertTrue((self.base / "restart-sink").exists())

    def test_tone_without_fifo_fails_without_blocking(self):
        start = time.monotonic()
        self.assertEqual(self.request("/tone", "POST")[0], 409)
        self.assertLess(time.monotonic() - start, 1)

    def test_native_tone_timeout_clears_exclusive_test_marker(self):
        script = self.base / "native-tone.sh"
        script.write_text('touch "' + str(self.base / "sink-test") + '"\nsleep 30\n')
        started = time.monotonic()
        try:
            self.assertEqual(self.request("/native-tone/clocked", "POST")[0], 409)
            self.assertLess(time.monotonic() - started, 7)
            self.assertFalse((self.base / "sink-test").exists())
        finally:
            script.unlink()

    def test_discovery_is_public_information_only(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.settimeout(1)
            udp.sendto(b"KINDLE_SPOTIFY/1?", ("127.0.0.1", 24880))
            self.assertEqual(udp.recvfrom(64)[0], b"KINDLE_SPOTIFY/1!")

    def test_log_redacts_private_key(self):
        key = "test-api-key-that-must-never-leak"
        (self.base / "private/soloist.key").write_text(key)
        (self.base / "player.log").write_text("diagnostic key=" + key + "\n")
        status, body = self.request("/log")
        self.assertEqual(status, 200)
        self.assertNotIn(key.encode(), body)

    def test_network_ports_are_owned_by_player_and_exclude_loopback(self):
        with socket.socket() as network, socket.socket() as local:
            network.bind(("0.0.0.0", 0)); network.listen(1)
            local.bind(("127.0.0.1", 0)); local.listen(1)
            (self.base / "player.pid").write_text(str(os.getpid()))
            ports = subprocess.check_output([str(self.binary), "--ports"], text=True).split()
            self.assertIn(str(network.getsockname()[1]), ports)
            self.assertNotIn(str(local.getsockname()[1]), ports)


if __name__ == "__main__":
    unittest.main()
