# Pulse shim 0.2.13

One `libpulse.so.0` that lets Spotify Soloist play through ALSA, with no
PulseAudio and no PipeWire on the system.

- [How it works](#how-it-works)
- [Quick start](#quick-start)
- [Running it as a service](#running-it-as-a-service)
- [Choosing the device](#choosing-the-device)
- [Volume](#volume)
- [Sharing the card with another player](#sharing-the-card-with-another-player)
- [Buffering and latency](#buffering-and-latency)
- [Diagnostics](#diagnostics)
- [Environment reference](#environment-reference)
- [What the library guarantees on every device](#what-the-library-guarantees-on-every-device)
- [What this library is not](#what-this-library-is-not)
- [Versions, archives and pinning](#versions-archives-and-pinning)
- [Troubleshooting](#troubleshooting)
- [Reporting a problem](#reporting-a-problem)
- [Building from source](#building-from-source)
- [Tests](#tests)
- [Notes from deployments](#notes-from-deployments)
- [Licence](#licence)

## How it works

Spotify's Soloist daemon has no ALSA output. It plays through the
PulseAudio client library: at startup it `dlopen`s `libpulse.so.0` and
looks up 48 `pa_*` functions by name, the list in [ABI.txt](ABI.txt). This
library is a `libpulse.so.0` that implements exactly those functions on
top of ALSA. Soloist writes its decoded audio into a Pulse stream as it
always does; the library puts that audio into the ALSA device you name,
reports playback position and underruns back the way a Pulse server
would, and nothing else runs.

It is loaded into one process, Soloist, through `LD_PRELOAD` or
`LD_LIBRARY_PATH`, from a directory of your choosing. It is never
installed as the system `libpulse.so.0`: any real PulseAudio client on
the machine keeps its own library, and the two never meet.

The library is not a Pulse server, not apulse and not a general
PulseAudio replacement. `libpulse-simple` and `libpulse-mainloop-glib` are
not built; Soloist does not use them. Runtime link is `libasound` and
libc only. The licence is MIT. Soloist itself is not part of it; you
fetch Soloist from Spotify as you do now.

The list in `ABI.txt` was taken from Soloist 1.3.7. Soloist builds through
1.3.10, including the ones running on piCorePlayer, moOde and Volumio,
load it. A later Soloist that looks up a name not in the list fails at
startup, and that name is the only reason the list ever grows.

## Quick start

Pick the archive that matches the Soloist binary, not the kernel: a
64-bit kernel often runs a 32-bit userspace.

```sh
file "$(command -v soloist)"
# ... ARM aarch64 ...         -> arm64
# ... ARM, EABI5 ...          -> armhf
# ... x86-64 ...              -> amd64
```

Download, verify, unpack:

```sh
cd ~
curl -LO https://github.com/foonerd/alsa_soloist_connect/releases/download/shim-v0.2.13/soloist-shim-0.2.13-arm64.tar.gz
curl -LO https://github.com/foonerd/alsa_soloist_connect/releases/download/shim-v0.2.13/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
mkdir -p ~/soloist-shim && tar -C ~/soloist-shim -xzf soloist-shim-0.2.13-arm64.tar.gz
ls ~/soloist-shim
# LICENSE  SOURCE_REVISION  libpulse.so.0
```

Find your card and start Soloist with the library preloaded:

```sh
aplay -L | grep -A1 '^plughw'
LD_PRELOAD=~/soloist-shim/libpulse.so.0 \
APULSE_PLAYBACK_DEVICE=plughw:CARD=sndallodigione,DEV=0 \
soloist --device-name "Living room" --api-key "$SPOTIFY_API_KEY" \
        --data-dir ~/soloist/data --cache-dir ~/soloist/cache
```

Play to the device from any Spotify app. Then confirm what Soloist
actually loaded:

```sh
pid=$(pidof soloist | tr ' ' '\n' | head -1)
grep libpulse /proc/$pid/maps | head -1       # must name your directory
cat ~/soloist-shim/SOURCE_REVISION            # the commit this build came from
tr '\0' ' ' < /proc/$pid/cmdline | grep -o -- '--api-key [^ ]*'
# --api-key nice-try-logsubmit               (the real key is gone from argv)
```

`LD_PRELOAD` is the better of the two loading methods: the library then
runs from the first instruction, which is what lets it remove the API
key from the command line before anything can read it. With
`LD_LIBRARY_PATH` alone, Soloist loads the library only when it first
plays, and an idle daemon shows the key in `ps` until then.

## Running it as a service

Soloist takes the API key only as a command-line argument. Keep the key
out of the unit file and out of the environment of the running process:
put it in a root-readable environment file, and use a small wrapper that
passes it as an argument and removes it from the environment before the
exec. The library overwrites the argument two seconds after load.

`/etc/soloist/soloist.env` (mode 0600):

```sh
API_KEY=your-key-from-developer.spotify.com
DEVICE_NAME=Living room
PLAYBACK_DEVICE=plughw:CARD=sndallodigione,DEV=0
```

`/usr/local/bin/soloist-run`:

```sh
#!/bin/sh
set -a; . /etc/soloist/soloist.env; set +a
SHIM=/opt/soloist-shim
exec env -u API_KEY \
  LD_PRELOAD="$SHIM/libpulse.so.0" \
  APULSE_PLAYBACK_DEVICE="$PLAYBACK_DEVICE" \
  /usr/local/bin/soloist \
    --device-name "$DEVICE_NAME" --api-key "$API_KEY" \
    --data-dir /var/lib/soloist/data --cache-dir /var/lib/soloist/cache \
    --ws 127.0.0.1:9878 --initial-volume 50 --cache-size 1024
```

`/etc/systemd/system/soloist.service`:

```ini
[Unit]
Description=Spotify Soloist (ALSA)
After=network-online.target sound.target
Wants=network-online.target

[Service]
Type=simple
User=soloist
Group=audio
ExecStart=/usr/local/bin/soloist-run
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

To turn diagnostics on for one run without editing anything, a drop-in
is enough:

```sh
sudo systemctl edit soloist.service
# [Service]
# Environment=APULSE_DIAG=1
sudo systemctl restart soloist.service
journalctl -u soloist.service -f
```

The library's lines arrive on stderr prefixed `soloist-shim:`, interleaved
with Soloist's own, so one journal holds both sides.

## Choosing the device

`APULSE_PLAYBACK_DEVICE` is the ALSA device string, exactly what you would
give `aplay -D`. The library opens it with the format and rate Soloist
delivers, 32-bit float at the track's rate, and does not negotiate
anything else: if the device refuses that format or that exact rate, the
open fails and the diagnostics say which. That is deliberate, and it
means one thing for the device string: put a `plug` in front of the
hardware, so ALSA converts the format and resamples when the card needs
it.

| Device string | When |
|---|---|
| `plughw:CARD=sndallodigione,DEV=0` | a card by name, the usual choice; `aplay -L` lists the names |
| `plughw:1,0` | the same card by number; numbers can change across boots, names do not |
| `plug:dmix:CARD=sndrpihifiberry` | the card shared with other ALSA players through `dmix`; the library's stream is mixed with theirs |
| `_audioout` | a named PCM from your own `asound.conf` chain (moOde's output chain), when that chain already converts |
| `default` | whatever ALSA's default resolves to on the host; fine on a desktop, rarely what you want on a player |
| `hw:CARD=X,DEV=0` | only if the card itself takes float at every Spotify rate; most do not, and the open then fails with `pcm open ... failed` |

Two facts behind the table:

- Spotify material is 44.1 kHz, and some podcasts and some tracks are
  48 kHz. A card fixed at 48 kHz refuses 44.1 kHz without `plug`; the
  diagnostic line is `pcm rate 44100 refused (near=48000)`. With
  `plughw` the same card plays everything.
- The library never picks S16 or S32 for you. On a chain that includes
  software plugins, choosing a narrower format at this point can become
  the format the hardware opens, and the stream then runs at the wrong
  speed. `plug` picks the card's native format and converts.

If the card is in use by another program, the library waits and retries
rather than failing; see [Sharing the card](#sharing-the-card-with-another-player).

## Volume

Soloist receives the volume set in the Spotify app and tells the library
through the Pulse API. What happens next is your choice.

**The library scales the samples (default).** Leave
`APULSE_EXTERNAL_VOLUME` unset. The Spotify volume slider works; the
card's mixer is untouched. This is software volume applied in float
before conversion, so it costs nothing in quality at normal listening
levels. `--initial-volume` on Soloist's command line sets where the
slider starts.

**Your mixer owns the fader.** Set `APULSE_EXTERNAL_VOLUME=1`. The
library passes the samples at full scale and ignores the Spotify volume.
Use this when the DAC or your system has a hardware or software mixer
you prefer, and drive that mixer from Soloist's volume events (Soloist
publishes them on its WebSocket API, the `--ws` address). If nothing
drives the mixer, the Spotify slider does nothing: that is the expected
consequence, not a fault.

**A fixed offset.** `APULSE_OUTPUT_TRIM_DB=-6` applies a constant gain
before the write, between -12 and +12 dB, independent of the volume
model above. Use it to level-match Spotify to your other sources. Values
above 0 can clip loud material; the library does not limit.

## Sharing the card with another player

Two players cannot open one ALSA card at the same time. There are two
ways to live with that.

**Mix.** Point the library at `plug:dmix:CARD=...` and let every player
do the same. ALSA mixes, nobody waits, and the card is never handed
over. This is the simplest answer when all players on the box can use
`dmix`.

**Hand the card over on request.** This is what `APULSE_YIELD_PATH` is
for. It names a file. When that file exists, the library closes the
card; when the file is gone and Soloist next asks to play, the library
opens the card again. Your other player, or the script that starts it,
owns the file:

```sh
# before starting the other player
touch /run/soloist/release-card
sleep 0.3                      # the library closes on its next wakeup
squeezelite -o hw:CARD=sndrpihifiberry ...

# when the other player has finished
rm -f /run/soloist/release-card
```

What happens, in order:

1. The library sees the file on its next wakeup and closes the card. The
   diagnostic is `pcm close handed off keep=1`. Audio already queued
   inside the library at that moment is dropped; the position Soloist
   sees is held where playback stopped. Pause Spotify before you hand
   over if that matters.
2. While the file exists, every attempt by Soloist to play is deferred.
   The library checks for the file every 50 ms and does not touch the
   card.
3. Once the file is gone, the next write or resume from Soloist reopens
   the card. If the other player still holds it, the library retries
   with a back-off of 50, 100, 200, 400 and then 800 ms, for as long as
   Soloist wants to play. The diagnostics name the process that holds
   the card: `pcm wait plughw:... holder=squeezelite` once, and after
   seven waits `pcm open ... failed: Device or resource busy holder=...`
   once. It keeps retrying.

The library never creates or removes the file. With the variable unset
there is no hand-over: the card stays open from the first sound until
Soloist disconnects, which is the right setting for a box where Soloist
is the only player or where `dmix` does the sharing.

## Buffering and latency

Soloist asks the Pulse side for a target buffer length, `tlength`, and
the library sizes its own ring and the ALSA buffer from that. A larger
buffer rides out hiccups; a smaller one makes pause, seek and the volume
slider react sooner. The diagnostic line printed when Soloist opens its
stream shows the figure in bytes:

```text
soloist-shim: connect corked=1 yield=0 tlength=705600 minreq=35280 rate=44100
```

At 44.1 kHz stereo float, 352800 bytes is one second, so that request is
two seconds. `APULSE_MAX_TLENGTH_MS=500` caps it at half a second;
Soloist's request stands when the variable is unset. A cap is worth
setting on a chain whose own latency already adds to the total, and on a
box where a two-second pause before the slider takes effect is a
complaint. Below about 200 ms the buffer no longer absorbs a busy CPU.

The ALSA period is chosen near Soloist's `minreq`, and while the card is
running the library writes at most two periods per wakeup whatever the
card reports free, so a chain that advertises a large free space is not
filled in one go.

## Diagnostics

`APULSE_DIAG=1` turns on the library's diagnostic lines on stderr, each
prefixed `soloist-shim:`. Unset, the library prints nothing at all. The
variable is read once at load.

A healthy start, annotated:

```text
soloist-shim: cmdline --api-key scrubbed (37 bytes)      key removed from argv, two seconds after load
soloist-shim: pa_context_new                              Soloist's Pulse calls, by name
soloist-shim: context ready
soloist-shim: pa_stream_new_with_proplist
soloist-shim: connect corked=1 yield=0 tlength=705600 minreq=35280 rate=44100
                                                          the stream and the buffer Soloist asked for
soloist-shim: cork 0 pcm=0                                Soloist started playback (cork 0 = play, 1 = pause)
soloist-shim: pcm open plughw:CARD=sndallodigione,DEV=0 period=4410 buffer=88200 rate=44100 client_fmt=5 alsa=FLOAT_LE
                                                          the card opened: period and buffer in frames, the format written
soloist-shim: write 32768 -> 32768 queued=32768 paused=0 pcm=1
                                                          the first eight writes are logged, then one line says they stop
soloist-shim: pa_stream_write (further calls not logged)
```

Every line the library can print:

| Line | Meaning |
|---|---|
| `cmdline --api-key scrubbed (N bytes)` | the API key was overwritten on Soloist's command line |
| `context ready` | Soloist connected its Pulse context |
| `connect corked=C yield=Y tlength=T minreq=M rate=R` | Soloist opened its stream; `T` and `M` in bytes after any cap; `Y` is 1 when a hand-over file is present at that moment |
| `pcm open DEV period=P buffer=B rate=R client_fmt=F alsa=FMT` | the card opened; `P` and `B` in frames; `FMT` the format written |
| `pcm wait DEV holder=NAME` | the card is busy; retrying; `NAME` is the process holding it |
| `pcm open DEV failed: Device or resource busy holder=NAME` | still busy after seven retries; retrying every 800 ms |
| `pcm open DEV failed: ERR` | the device string, the format or the rate was refused |
| `pcm rate R refused (near=N)` | the card does not do rate `R`; use `plughw` or `plug` |
| `cork 1 pcm=1` / `cork 0 pcm=1` | pause / play; `pcm=1` means the card is open |
| `flush ring pcm=1 drop_unsafe=0 avail=N` | a seek or a track change discarded the queued audio |
| `write N -> M queued=Q paused=P pcm=C` | one of the first eight writes; `M < N` is a short write, normal when the ring is nearly full |
| `ring dry, pcm STATE delay=D wait=Wms` | Soloist stopped supplying audio; the card still holds `D` frames (`-1`: it would not say); the library looks again in `W` ms |
| `underflow after N ms, pcm STATE delay=D` | the dry spell was real; Soloist has been told, once, `N` ms after the ring ran dry |
| `poll error, pcm XRUN` | the card underran; recovered with one prepare, position held |
| `avail ERR` / `writei ERR, avail=N` | an ALSA error on the I/O path; recovered with one prepare |
| `pcm prepare failed` / `pcm dead, reopen scheduled` | the handle is dead; closed and reopened away from the I/O thread |
| `pcm close handed off keep=1` | the card was released on request, position held; `keep=0` on disconnect |
| `stream disconnect` | Soloist closed the stream |
| `pa_NAME` | a Pulse call Soloist made; the frequent ones log eight times, then `(further calls not logged)` |
| `mainloop stop` | shutdown |

Soloist's own lines worth knowing, with `-v`: `eof at` and `written eos
to driver` at the end of a track, `is buffering` and `Starting playback`
around a stop and a start, `tryStartPlayback failed` while it waits to
have enough audio to start.

A dry spell that recovered, which is what a brief stall on the Soloist
side looks like now:

```text
soloist-shim: ring dry, pcm RUNNING delay=3300 wait=94ms
soloist-shim: poll error, pcm XRUN
soloist-shim: underflow after 94 ms, pcm PREPARED delay=0
soloist-shim: pa_stream_cork
soloist-shim: cork 1 pcm=1
...
soloist-shim: cork 0 pcm=1
```

## Environment reference

| Variable | Meaning | Unset means | Example |
|---|---|---|---|
| `APULSE_PLAYBACK_DEVICE` | the ALSA device string | `plug:volumio`; set it on any other host | `plughw:CARD=sndallodigione,DEV=0` |
| `APULSE_YIELD_PATH` | hand-over file: the card is closed while the file exists | no hand-over; the card stays open until Soloist disconnects | `/run/soloist/release-card` |
| `APULSE_MAX_TLENGTH_MS` | cap on the buffer length Soloist asks for | Soloist's request stands | `500` |
| `APULSE_EXTERNAL_VOLUME` | `1`: pass samples at full scale, your mixer owns the fader | the library scales the samples to the Spotify volume | `1` |
| `APULSE_OUTPUT_TRIM_DB` | fixed gain in dB, -12 to +12, applied before the write | 0 dB | `-6` |
| `APULSE_DIAG` | diagnostic lines on stderr | quiet | `1` |

The names are historical and stay as they are; every variable is read
once, at load or at the first stream.

## What the library guarantees on every device

Soloist sees one Pulse stream and does not know whether the descriptor
underneath is a kernel PCM or a software plugin. The behaviour it can
rely on is the same for both.

- The open format is the client format at the client rate. Exact rate or
  the open fails. A `plug` in the device string is the converter; this
  library does not pick S16 or S32 and does not `set_rate_near`.
- The played position is in client bytes: `write_index` minus the ring
  minus `snd_pcm_delay` on the PCM this library opened. A finished frame,
  including the last frame of a stream, is reported exactly. The read
  index never goes backwards.
- One underflow per dry spell, when the ring is empty and the device
  holds nothing, including a dry spell the poll descriptor never reports.
  The library waits for what the device says it still holds, plus 20 ms,
  then looks: a device that has stopped or reports nothing left is dry; a
  device still running whose delay has moved since the last look, down as
  it consumes or up by a frame as a pointer estimate steps back, is
  playing, so the new figure is waited out and the look repeated; one
  whose delay is exactly where it was is a software plugin with a stuck
  figure, and it is dry. A device that will not say gets the whole buffer; no spell
  lasts longer than twice the buffer. The next write that queues data
  ends the spell, as does flush. A poll error on the descriptor goes
  through the same recovery as any underrun: position sampled, clock
  frozen, one prepare, a line in the diag. The next unpaused write
  restarts a `PREPARED` device.
- `pa_stream_write` accepts a short prefix when the ring cannot take the
  whole buffer. Soloist writes 32 KiB and will not uncork if the call
  fails.
- Cork keeps the PCM and feeds silence while it runs. Flush drops the
  ring and keeps the handle. The PCM is closed only on hand-over or
  disconnect, never from the I/O callback.
- While running, one wakeup writes at most two periods.
- `--api-key` on Soloist's command line is overwritten two seconds after
  load, and on the first `pa_*` call, so `ps` and log uploads do not show
  the secret.

## What this library is not

A request for exclusive `hw:`, no float, no plug, no resampling, DSD, or
a second client is not another function and not another host. Soloist
hands this library a Pulse stream; the library writes that stream's
format at that stream's rate, and `plug` converts to what the card
opens. A DAC showing 44100 Hz `S24_LE` under a float stream is `plughw`
converting. Bit-perfect from Spotify is not available on this path. The
controls that do belong to you are the device string and
`APULSE_EXTERNAL_VOLUME`. A request that needs a Pulse server or a
bit-perfect hop is a different product.

## Versions, archives and pinning

A release is a tag `shim-v<version>` on this repository, built by CI
from that commit in Debian 12 containers, the ARM ones under QEMU. The
artefact is `soloist-shim-<version>-<arch>.tar.gz`, one per userspace:
`amd64` (x86_64), `arm64` (aarch64), `armhf` (armv7l). Each archive holds
`libpulse.so.0`, `SOURCE_REVISION` and `LICENSE`; `SHA256SUMS` on the
release page carries the digests.

What you pin is the tag and the `SOURCE_REVISION` inside the archive;
they identify the bytes. The soname is `libpulse.so.0` and does not
change. `SOVERSION` is 0. The version of any product that bundles this
library is not this library's version.

The built library needs glibc 2.34 or later. Every system that runs the
official Soloist binary natively already has a newer one, so this is
never the constraint.

Releases are published as pre-releases and promoted once confirmed on a
hardware PCM and on a software-plugin chain. The notes on each release
page say which. Changes are in [CHANGELOG.md](CHANGELOG.md).

To check a running Soloist against a release:

```sh
pid=$(pidof soloist | tr ' ' '\n' | head -1)
lib=$(grep -m1 -o '/[^ ]*libpulse.so.0' /proc/$pid/maps)
sha256sum "$lib"; cat "$(dirname "$lib")/SOURCE_REVISION"
```

and compare with the archive you unpacked.

## Troubleshooting

| Symptom | Likely cause | What to do |
|---|---|---|
| Spotify shows the device, playing shows no sound, diag says `pcm open ... failed: No such file or directory` | the device string names nothing | `aplay -L`; use the `plughw:CARD=...` form |
| `pcm rate 44100 refused (near=48000)` | the card is fixed at another rate and there is no `plug` | `plughw:` instead of `hw:` |
| `pcm open ... failed: Invalid argument` | the card refuses float, or the channel count | `plughw:` instead of `hw:` |
| `pcm wait ... holder=NAME` then `Device or resource busy` | another player holds the card | `dmix`, or the hand-over file around the other player |
| Sound at the wrong speed, pitched | a chain in front of the card opened a different rate or format | give the library the `plughw` of the card, not a chain that renegotiates |
| The Spotify slider does nothing | `APULSE_EXTERNAL_VOLUME=1` with nothing driving the mixer | unset it, or drive the mixer from Soloist's volume events |
| Too loud or too quiet against other sources | levels differ by design | `APULSE_OUTPUT_TRIM_DB` |
| A track plays to the end and never advances; Soloist logs `Starting playback` every 5 s | a library older than 0.2.10 | update |
| Audio stops mid-track, Soloist logs `tryStartPlayback failed` every 200 ms, the card reads `PREPARED` | a library older than 0.2.11 | update; if it recurs on 0.2.13, report with the `ring dry` and `underflow` lines |
| `ps` shows the real API key | the library was not loaded at exec | use `LD_PRELOAD`, check the path; the diag's first line confirms the scrub |
| `symbol lookup error` or `undefined symbol: pa_...` at startup | a Soloist newer than the ABI list | report the name; see [How it works](#how-it-works) |
| Silence after another player used the card | the hand-over file is still present | remove it; the library reopens on Soloist's next play |

## Reporting a problem

Open an issue on this repository with:

- the platform, the board, the DAC, and the Soloist version from
  `soloist --version`;
- the two confirmation lines from [Quick start](#quick-start), so the
  report is tied to a build;
- `APULSE_PLAYBACK_DEVICE` and the relevant part of your `asound.conf`
  if the device is a named chain;
- the diagnostics: Soloist started with `-v` and `APULSE_DIAG=1`, the
  thirty seconds before the event through the event itself, Soloist's
  lines and the `soloist-shim:` lines together. Redact nothing else;
  the key is already gone from them.

## Building from source

Plain CMake 3.13 or later with `libasound2-dev`:

```sh
cmake -S shim -B build && cmake --build build
ls build/libpulse.so.0*
```

The Docker matrix builds the three architectures the releases ship, in
the same images CI uses:

```sh
./build-matrix.sh                 # all three
./docker/run-docker-shim.sh amd64 # one
```

Output lands in `out/<arch>/` and, for the Volumio plugin's tree, in
`soloist_connect/alsa-lib/<arch>/`. `scripts/shim-release.sh archive
<arch>` turns an `out/<arch>/` built from a clean checkout into the
release archive.

## Tests

Regression tests are opt-in (`BUILD_TESTING=ON`) and build no part of
the library. To run them without changing anything:

```sh
docker run --rm --network none -v "$PWD:/src:ro" soloist-shim-builder:amd64 \
  bash /src/scripts/check-shim.sh
```

Five groups: exact completion across formats, rates and channel counts;
the stream lifecycle (cork, flush, hand-over, reclaim, recovery); timing
precision; drain and recovery; and the dry spell (one underflow per
spell, on a stopped device, on a running device that reports nothing
left, on a running device past the bound, re-arm on write and flush,
nothing while corked or without a PCM, the poll-error recovery). They use
controlled ALSA responses without opening an audio device. CI runs them
in the same container on every push, and the release workflow runs them
for each architecture before it archives the library.

GNU AddressSanitizer builds use non-PIE linking for the test executable;
the shared library remains position-independent.

Related: [issue #1: tracks never end outside Volumio](https://github.com/foonerd/alsa_soloist_connect/issues/1).

## Notes from deployments

Why the guarantees above are shaped as they are. The devices named are
where each rule was learned; the rule holds everywhere.

- writei the client spec (FLOAT32 at the client rate). Do not pick S16
  or S32: on volumioswitch that becomes the USB/AML open and rushes.
  The `plug:` in `plug:volumio` is the converter. Exact rate or fail;
  never `set_rate_near` onto 48 k / 88.2 k. I/O is capped at two
  periods so switcher `avail` is not treated as the DAC. Packed
  S24_3LE is never the client format. Bit-perfect is not possible.
- Played time is `write_index` minus ring fill minus `snd_pcm_delay` on
  the PCM we opened. `/proc/asound` is not scanned for some other card.
- Played, frozen and held positions are counted in client bytes. Timing
  indices do not round-trip through microseconds; a completely played
  frame is reported exactly, including the final frame of a stream
  (0.2.10, issue 1: the microsecond round trip left the read index one
  frame short at 440 of every 441 positions at 44.1 kHz, and Soloist
  waited for that frame forever).
- Active XRUN recovery snapshots played bytes before preparing the PCM
  and freezing the clock. Ring-buffered data remains outstanding; paused
  streams and other recovery states retain their held positions.
- A dry ring is reported as one underflow once ALSA has played out
  (0.2.11, issue 1). Before, the ring running dry only parked the output,
  the mainloop prepared the underrun it then saw on the descriptor without
  a word, and a Soloist that had been told the stream was drained but
  never told underflow stopped feeding and never came back. 0.2.11 decided
  on a fixed bound, which called a device still playing at the bound dry
  and waited a whole buffer on a device that had said it was empty; 0.2.12
  waits out a delay that is shrinking and calls only an unmoved delay
  stuck, zero delay is the margin alone, and twice the buffer is the cap;
  0.2.13 treats a delay that rose by a frame, a pointer estimate stepping
  back on a card still playing, as moving too.
- `pa_stream_write` takes a prefix if the ring cannot hold the whole
  buffer. Soloist always writes 32 KiB and will not uncork if that call
  fails, so rejecting a short write deadlocks preroll (0.2.1).
  The ring is `tlength` plus 64 KiB so a default 500 ms cap does not
  drop the tail of that write (0.2.4 on Rivo/Integro).
- Two seconds after load, and on the first `pa_*` call, `--api-key` on
  argv is overwritten with `nice-try-logsubmit` so `ps` and logsubmit do
  not publish the secret. The constructor must not scrub: it runs before
  `main()` parses argv.
- Pulse `tlength` / `minreq` pace Soloist. The ALSA period is chosen with
  `snd_pcm_hw_params_set_period_size_near`.
- Cork keeps the PCM. Close only when the hand-over file appears or the
  stream disconnects. `snd_pcm_close` is never run from the I/O callback.
- Flush discards the Pulse ring and resets the write/read indices. It
  does not `snd_pcm_drop` or `prepare`. Drop on a healthy Motivo
  MultiRoom handle killed `volumioOutput` on the second seek. Committed
  ALSA audio plays out to the output-buffer bound. A dead handle
  (`drop_unsafe` or a hard `avail` error) still reopens.
- `pa_context_connect` sets CONNECTING then READY in the same call.
- The ring is sized in time at the client's frame size.
  `pa_stream_writable_size` is the room left against `tlength`.
- An xrun is `snd_pcm_prepare` with the playhead held, not a close.
  `prepare` plus a still-dead `avail` is a dead switcher target: reopen,
  deferred off the I/O callback.

## Licence

MIT, see [LICENSE](../LICENSE), which also travels inside every release
archive. The PulseAudio headers under [include/pulse/](include/pulse/)
are LGPL 2.1 or later and are used at build time only; the attribution
is in [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md).
