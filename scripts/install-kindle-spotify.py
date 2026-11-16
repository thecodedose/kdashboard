#!/usr/bin/env python3
"""Install the direct player while preserving private Spotify session data."""
from pathlib import Path
import datetime
import hashlib
import secrets
import shutil
import argparse
import ipaddress
import re

root = Path(__file__).resolve().parents[1]
source = root / "kindle/native/build/spotify-direct/extension"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--volume", type=Path, default=Path("/Volumes/Kindle"))
parser.add_argument("--speaker-mac", help="Address of your already paired Bluetooth speaker")
parser.add_argument("--debug-host", help="Optional desktop IPv4 for diagnostics")
args = parser.parse_args()
volume = args.volume
existing_speaker = volume / "extensions/kindle-spotify/private/speaker.mac"
speaker = args.speaker_mac or (existing_speaker.read_text().strip() if existing_speaker.exists() else "")
if not re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", speaker):
    parser.error("Supply --speaker-mac for your paired speaker on first installation.")
debug_host = str(ipaddress.IPv4Address(args.debug_host)) if args.debug_host else None
assert (volume / "documents").is_dir(), "Connect the Kindle by USB first."
assert (source / "bin/soloist").is_file(), "Build the player first."
key = (root / ".spotify/soloist.key").read_text().strip()
assert len(key) >= 16 and not any(c.isspace() for c in key), "Missing private Soloist API key."
target = volume / "extensions/kindle-spotify"
if target.exists():
    backup = root / "kindle/native/build" / ("before-direct-spotify-" + datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    shutil.copytree(target, backup, copy_function=shutil.copyfile)
shutil.copytree(source, target, dirs_exist_ok=True, copy_function=shutil.copyfile,
                ignore=shutil.ignore_patterns("private", "config.sh", "*.log", "*.pid"))
private = target / "private"
private.mkdir(mode=0o700, exist_ok=True)
private.chmod(0o700)
(private / "soloist.key").write_text(key + "\n")
(private / "soloist.key").chmod(0o600)
(root / ".spotify").chmod(0o700)
token_path = root / ".spotify/kindle-direct-token"
if not token_path.exists():
    token_path.write_text(secrets.token_urlsafe(32))
token_path.chmod(0o600)
(private / "control.token").write_bytes(token_path.read_bytes())
(private / "control.token").chmod(0o600)
if debug_host:
    (private / "debug-host").write_text(debug_host)
    (private / "debug-host").chmod(0o600)
(private / "speaker.mac").write_text(speaker.upper() + "\n")
(private / "speaker.mac").chmod(0o600)
if not (private / "sink.mode").exists():
    (private / "sink.mode").write_text("kinamp-clocked\n")
for name in ["start.sh", "stop.sh", "daemon.sh"]:
    (target / name).chmod(0o755)
for name in ["bin/soloist", "bin/spotify-control", "bin/spotify-mixer", "lib/ld-linux-armhf.so.3"]:
    assert hashlib.sha256((source / name).read_bytes()).digest() == hashlib.sha256((target / name).read_bytes()).digest()
hook = '''  # Start the optional direct Spotify player alongside the dashboard.
  if [ -f /mnt/us/extensions/kindle-spotify/start.sh ]; then
    /bin/sh /mnt/us/extensions/kindle-spotify/start.sh quiet </dev/null >/dev/null 2>&1 &
  fi
'''
launchers = [(volume / "extensions/kindle-dashboard/bin/dashboard.sh", '  nohup "$RUN_APP"')]
launchers += [(volume / "documents" / name, '  exec "$RUN_APP"') for name in
              ["kindle-dashboard-launch.sh", "kindle-dashboard-launch-light.sh", "kindle-dashboard-launch-dark.sh"]]
for path, marker in launchers:
    if not path.exists():
        continue
    text = path.read_text()
    if "# Start the optional direct Spotify player" not in text:
        assert marker in text, "Unrecognized launcher: " + path.name
        path.write_text(text.replace(marker, hook + marker, 1))
print("Installed Kindle Spotify and verified executable hashes. Private values withheld.")
