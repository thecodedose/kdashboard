"""Linux checks for continuous native PCM output, pacing and cleanup."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux PCM helper")
class NativeMixer(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="kindle-mixer-")
        self.base = Path(self.temp.name)
        os.mkfifo(self.base / "audio.fifo")
        self.gst = self.base / "fake-gst"
        self.binary = self.base / "mixer"
        source = Path(__file__).resolve().parents[1] / "kindle/native/src/spotify_mixer.c"
        subprocess.run(["gcc", "-O2", "-Wall", "-Wextra", "-Werror",
                        '-DRUN="' + str(self.base) + '"', '-DGST_LAUNCH="' + str(self.gst) + '"',
                        str(source), "-o", str(self.binary)], check=True)
        self.process = None

    def tearDown(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=2)
        self.temp.cleanup()

    def start(self, stalled=False):
        script = "#!" + sys.executable + "\nimport os,sys,time\nfrom pathlib import Path\n"
        script += "Path(" + repr(str(self.base / "gst.pid")) + ").write_text(str(os.getpid()))\n"
        if stalled:
            script += "time.sleep(30)\n"
        else:
            script += "with open(" + repr(str(self.base / "output.raw")) + ", 'wb', buffering=0) as out:\n"
            script += " while True:\n  b=os.read(0,1764)\n  if not b:break\n  out.write(b)\n"
        self.gst.write_text(script)
        self.gst.chmod(0o700)
        self.process = subprocess.Popen([str(self.binary)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if (self.base / "gst.pid").exists():
                return
            time.sleep(0.01)
        self.fail("Native sink did not start")

    def stop(self):
        started = time.monotonic()
        self.process.terminate()
        self.assertEqual(self.process.wait(timeout=2), 0)
        self.assertLess(time.monotonic() - started, 1)
        pid = int((self.base / "gst.pid").read_text())
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)

    def test_silence_audio_pause_and_pacing(self):
        self.start()
        begun = time.monotonic()
        time.sleep(0.35)
        fd = os.open(self.base / "audio.fifo", os.O_WRONLY | os.O_NONBLOCK)
        block = struct.pack("<hh", 4660, 4660) * 441
        for _ in range(100):
            self.assertEqual(os.write(fd, block), len(block))
            time.sleep(0.01)
        os.close(fd)
        time.sleep(0.25)
        elapsed = time.monotonic() - begun
        self.stop()
        data = (self.base / "output.raw").read_bytes()
        self.assertGreater(len(data), elapsed * 176400 * 0.75)
        self.assertLess(len(data), elapsed * 176400 * 1.25)
        samples = struct.unpack("<" + str(len(data) // 2) + "h", data)
        self.assertTrue(all(n in {0, 4660} for n in samples))
        self.assertEqual(samples.count(4660), 88200)
        self.assertEqual(data[:1764 * 10], bytes(1764 * 10))
        self.assertEqual(data[-1764 * 10:], bytes(1764 * 10))
        stats = (self.base / "mixer.stats").read_text()
        self.assertIn("silence_bytes=", stats)

    def test_stalled_native_sink_remains_stoppable(self):
        self.start(stalled=True)
        time.sleep(0.4)
        self.assertIsNone(self.process.poll())
        self.stop()


if __name__ == "__main__":
    unittest.main()
