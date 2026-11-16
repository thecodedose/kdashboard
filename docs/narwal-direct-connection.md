# Narwal X10 Pro direct connection test

This standalone KUAL extension tests the native Kindle → local Wi-Fi → Narwal
Freo X10 Pro (AX15) connection. It does not require Home Assistant, a cloud
backend, a Narwal account, or a companion computer after installation.

The test opens port 9002, verifies the WebSocket upgrade, and tries to receive
the X10 Pro's identity and live battery telemetry. It saves the device identity
locally so later queries can address the vacuum while its broadcast feed is
quiet. It sends only identity/status
queries and passive, short-lived status subscriptions. It has no cleaning,
return-to-dock, map, camera, or settings commands. Dashboard controls are not
implemented by this diagnostic. The dashboard now has a separate
[vacuum control card](kindle-widgets.md#vacuum-controls).

Protocol source: <https://github.com/sjmotew/NarwalIntegration/blob/master/docs/PROTOCOL.md>.
This is an unofficial local protocol, so firmware compatibility must be tested
on the actual unit. A WebSocket connection alone does not prove live status or
cleaning controls work.

## Build and check

```sh
make -C kindle/native check-narwal
make -C kindle/native narwal-probe-zig ZIG=/path/to/zig
```

The ARM binary is static and has no Python, Node, Home Assistant, or system
WebSocket-library dependency. The native tests exercise the HTTP upgrade proof,
masked client frames, fragmented status with ping/pong, timeouts, message size
limits, truncated protobuf, invalid battery data, and redaction of device IDs.

The KUAL launcher copies the executable to a private directory under `/tmp`
before running it, because Kindle user storage may be mounted with `noexec`.
The temporary executable is removed when the test finishes.

For a desktop connection test:

```sh
kindle/native/build/narwal-probe-local VACUUM_IPV4 --timeout 30 --identity-file kindle/native/build/narwal-identity.txt
```

## Kindle installation and use

1. Copy `kindle/kual/narwal-probe/` to the mounted Kindle's `extensions/` folder.
2. Copy `kindle/native/build/narwal-probe-arm` to
   `extensions/narwal-probe/bin/narwal-probe`.
3. Copy `config.sh.example` to `config.sh`, and set `NARWAL_HOST` to the vacuum's
   local IPv4 address. This device-local file is ignored by Git. Reserve the
   address in the router if you need it to remain stable.
4. Make `bin/test.sh` and `bin/narwal-probe` executable.
5. Eject and disconnect USB; turn airplane mode off and connect to the same
   local network as the vacuum. Guest-network isolation may block access.
6. Open KUAL → **Narwal Connection Test** → **Test X10 Pro (status only)**.
   Allow up to 30 seconds. Avoid tapping the test twice simultaneously.
7. Reconnect USB to inspect `documents/narwal-connection-test.txt`.

The device identity is saved in `extensions/narwal-probe/identity.txt`, which
the launcher passes to the native probe. A unit that ignores discovery while
idle can be set up once with its captured identity; later runs use addressed
status queries and decode their responses even without broadcasts.

The log reports exit 0 only after live battery telemetry is decoded. Exit 2
means the WebSocket opened but live status could not be confirmed; exit 1 means
the connection/upgrade failed. Device IDs, account IDs, full payloads, and maps
are not written to the log.

Some idle units do not immediately publish status. Briefly open the vacuum's
page in the Narwal app to wake its status feed, close the app, then repeat the
test. The connection timeout is bounded and no persistent service is installed.

To remove the test, delete `extensions/narwal-probe/`. If the dashboard launcher
is using its configuration fallback, first copy the identity to the dashboard
folder and set `NARWAL_HOST` / `NARWAL_IDENTITY_FILE` in the dashboard's own
`config.sh`. The installed vacuum-card update does this during installation.
