#!/usr/bin/env python3
"""Control the prototype's authenticated, fixed-command diagnostic interface."""
import argparse
import ipaddress
from pathlib import Path
import socket
import sys
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def discover(timeout=8):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        udp.settimeout(1)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            udp.sendto(b"KINDLE_SPOTIFY/1?", ("255.255.255.255", 24880))
            try:
                message, peer = udp.recvfrom(64)
            except socket.timeout:
                continue
            if message == b"KINDLE_SPOTIFY/1!":
                return peer[0]
    raise RuntimeError("Kindle player not found. Start it in KUAL with USB unplugged.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["health", "status", "now", "log", "sink-log", "supervisor-log", "mixer-log", "metrics", "audio-status", "audio-focus", "audio-volume", "reconnect-speaker", "tone", "native-tone", "sink-mode", "play", "pause", "next", "previous", "restart", "shutdown", "volume", "playlist"])
    parser.add_argument("value", nargs="?")
    parser.add_argument("--host")
    parser.add_argument("--tail", type=int, default=60)
    args = parser.parse_args()
    host = str(ipaddress.IPv4Address(args.host or discover()))
    path = "/" + args.command
    if args.command in {"sink-mode", "native-tone"}:
        allowed = {"clocked", "unclocked", "kinamp", "kinamp-clocked"} if args.command == "sink-mode" else {"clocked", "unclocked"}
        if args.value not in allowed:
            parser.error(args.command + " requires clocked or unclocked")
        path += "/" + args.value
    if args.command in {"volume", "audio-volume"}:
        if args.value is None or not args.value.isdecimal() or not 0 <= int(args.value) <= 100:
            parser.error("volume requires a number from 0 to 100")
        path += "/" + str(int(args.value))
    if args.command == "playlist":
        if args.value is None or len(args.value) != 22 or not args.value.isascii() or not args.value.isalnum():
            parser.error("playlist requires its 22-character Spotify ID")
        path += "/" + args.value
    token = (ROOT / ".spotify/kindle-direct-token").read_text().strip()
    method = "GET" if args.command in {"health", "status", "now", "log", "sink-log", "supervisor-log", "mixer-log", "metrics", "audio-status"} else "POST"
    request = urllib.request.Request("http://" + host + ":24880" + path,
                                     headers={"X-Dashboard-Toggle-Token": token}, method=method)
    try:
        response = urllib.request.urlopen(request, timeout=10)
    except urllib.error.HTTPError as error:
        response = error
    body = response.read(65536).decode(errors="replace")
    key_file = ROOT / ".spotify/soloist.key"
    if key_file.exists():
        key = key_file.read_text().strip()
        if key:
            body = body.replace(key, "[PRIVATE KEY]")
    if args.command in {"log", "sink-log", "supervisor-log", "mixer-log"}:
        body = "\n".join(body.splitlines()[-max(1, args.tail):])
    print("Kindle:", host)
    print(body.strip())
    return 0 if response.status < 400 else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
