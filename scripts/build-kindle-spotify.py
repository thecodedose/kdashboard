#!/usr/bin/env python3
"""Build a private Kindle player bundle from pinned, official downloads."""
import argparse
import hashlib
import io
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "kindle/native/build/spotify-direct"
MANIFEST = json.loads((ROOT / "scripts/kindle-spotify-dependencies.json").read_text())


def download(url, destination, digest=None):
    if not destination.exists() or (digest and hashlib.sha256(destination.read_bytes()).hexdigest() != digest):
        with urllib.request.urlopen(url, timeout=60) as response:
            destination.write_bytes(response.read())
    data = destination.read_bytes()
    if digest and hashlib.sha256(data).hexdigest() != digest:
        raise RuntimeError("Download checksum mismatch: " + destination.name)
    return data


def deb_tar(data):
    if data[:8] != b"!<arch>\n":
        raise RuntimeError("Invalid Debian archive")
    offset = 8
    while offset + 60 <= len(data):
        header = data[offset:offset + 60]
        name = header[:16].decode().strip().rstrip("/")
        size = int(header[48:58])
        member = data[offset + 60:offset + 60 + size]
        offset += 60 + size + size % 2
        if name.startswith("data.tar"):
            return tarfile.open(fileobj=io.BytesIO(member))
    raise RuntimeError("Missing package data")


def patch_interpreter(path):
    data = bytearray(path.read_bytes())
    if data[:6] != b"\x7fELF\x01\x01":
        raise RuntimeError("Expected little-endian ARM32 ELF")
    phoff = struct.unpack_from("<I", data, 28)[0]
    entsize, count = struct.unpack_from("<HH", data, 42)
    interpreter = b"/var/tmp/kindle-spotify/lib/ld-linux-armhf.so.3\0"
    for i in range(count):
        offset = phoff + i * entsize
        header = list(struct.unpack_from("<8I", data, offset))
        if header[0] == 3:
            # Linux reads PT_INTERP directly from the ELF file, not a PT_LOAD mapping.
            header[1] = len(data)
            header[4] = header[5] = len(interpreter)
            struct.pack_into("<8I", data, offset, *header)
            data.extend(interpreter)
            path.write_bytes(data)
            path.chmod(0o755)
            return
    raise RuntimeError("No ELF interpreter")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig", default=os.environ.get("ZIG") or shutil.which("zig"))
    args = parser.parse_args()
    if not args.zig:
        parser.error("Pass --zig /path/to/zig")
    cache = BUILD / "downloads"
    include = BUILD / "include"
    package = BUILD / "extension"
    for directory in [cache, include, package / "bin", package / "lib", package / "licenses"]:
        directory.mkdir(parents=True, exist_ok=True)
    for source in (ROOT / "kindle/kual/kindle-spotify").iterdir():
        if source.is_file() and source.suffix in {".sh", ".xml", ".json", ".conf"} and source.name != "config.sh":
            shutil.copyfile(source, package / source.name)
    # Two-second low-volume tone plus a second of silence for the mixer tail.
    tone = bytearray()
    for frame in range(88200):
        sample = int(2600 * math.sin(2 * math.pi * 440 * frame / 44100))
        tone.extend(struct.pack("<hh", sample, sample))
    tone.extend(bytes(44100 * 4))
    (package / "test-tone.raw").write_bytes(tone)
    libnames = {"ld-linux-armhf.so.3", "libc.so.6", "libm.so.6", "libdl.so.2",
                "libpthread.so.0", "librt.so.1", "libresolv.so.2", "libnss_dns.so.2",
                "libnss_files.so.2", "libanl.so.1", "libutil.so.1"}
    for record in MANIFEST["packages"]:
        blob = download("https://deb.debian.org/debian/" + record["Filename"],
                        cache / Path(record["Filename"]).name, record["SHA256"])
        with deb_tar(blob) as archive:
            for member in archive.getmembers():
                if not member.isfile():
                    continue
                name = member.name.removeprefix("./")
                base = Path(name).name
                target = None
                if name.startswith("usr/include/alsa/"):
                    target = include / Path(name).relative_to("usr/include")
                elif base in libnames:
                    target = package / "lib" / base
                elif base.startswith("libatomic.so."):
                    target = package / "lib/libatomic.so.1"
                elif base.startswith("libasound.so.2"):
                    target = package / "lib/libasound.so.2"
                elif base == "copyright":
                    target = package / "licenses" / (record["Package"] + ".txt")
                if target:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(archive.extractfile(member).read())
                    target.chmod(member.mode)
    blob = download(MANIFEST["soloist_url"], cache / "soloist-arm32.tar.gz")
    with tarfile.open(fileobj=io.BytesIO(blob)) as archive:
        player = archive.extractfile("soloist").read()
        if hashlib.sha256(player).hexdigest() != MANIFEST["soloist_binary_sha256"]:
            raise RuntimeError("Spotify build changed; review and update the pinned manifest before building.")
        (package / "bin/soloist").write_bytes(player)
        for member in archive.getmembers():
            if member.isfile() and "LICENSE" in member.name.upper():
                (package / "licenses" / Path(member.name).name).write_bytes(archive.extractfile(member).read())
    patch_interpreter(package / "bin/soloist")
    vendor = ROOT / "kindle/native/vendor/soloist-pulse"
    command = [args.zig, "cc", "-target", "arm-linux-gnueabihf.2.38", "-mcpu=cortex_a7",
               "-O2", "-s", "-fPIC", "-fvisibility=hidden", "-shared", "-std=c99",
               "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Wno-unused-parameter",
               "-I" + str(vendor / "include"), "-I" + str(vendor / "src"), "-I" + str(include)]
    command += [str(vendor / "src" / (name + ".c")) for name in ["mainloop", "support", "context", "stream", "ring"]]
    command += [str(package / "lib/libasound.so.2"), "-Wl,-soname,libpulse.so.0",
                "-Wl,--version-script=" + str(vendor / "pulse.sym"), "-lm", "-lpthread",
                "-o", str(package / "lib/libpulse.so.0")]
    subprocess.run(command, check=True)
    subprocess.run([args.zig, "cc", "-target", "arm-linux-gnueabihf.2.38", "-mcpu=cortex_a7",
                    "-O2", "-s", "-shared", "-fPIC", "-DPIC", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(include), str(ROOT / "kindle/native/src/spotify_pcm.c"),
                    str(package / "lib/libasound.so.2"), "-o",
                    str(package / "lib/libasound_module_pcm_kindle_fifo.so")], check=True)
    subprocess.run([args.zig, "cc", "-target", "arm-linux-musleabi", "-mcpu=cortex_a7",
                    "-O2", "-s", "-static", "-Wall", "-Wextra", "-Werror",
                    str(ROOT / "kindle/native/src/spotify_control.c"), "-o", str(package / "bin/spotify-control")], check=True)
    subprocess.run([args.zig, "cc", "-target", "arm-linux-musleabi", "-mcpu=cortex_a7",
                    "-O2", "-s", "-static", "-Wall", "-Wextra", "-Werror",
                    str(ROOT / "kindle/native/src/spotify_mixer.c"), "-o", str(package / "bin/spotify-mixer")], check=True)
    shutil.copyfile(vendor / "LICENSE", package / "licenses/soloist-pulse.txt")
    (package / "BUILD.json").write_text(json.dumps(MANIFEST, indent=2) + "\n")
    print("Built private player bundle:", package)


if __name__ == "__main__":
    main()
