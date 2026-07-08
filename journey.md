# Kindle Paperwhite 7th Gen — Jailbreak + Custom Dashboard Journey

A writeup of jailbreaking a Kindle Paperwhite 7th generation (2015, PW3, codename
"wario") and installing [kdashboard](https://github.com/thecodedose/kdashboard), a
native C++ e-ink planner dashboard that polls a self-hosted backend and updates via
a Telegram bot, via KUAL. Documented as a reference for anyone doing the same thing
on similar hardware — especially the parts that went wrong and why, since most
existing guides only cover the happy path.

## Device Specs

- **Model**: Kindle Paperwhite 7th Generation (Amazon's own generation numbering) —
  this is the 2015 "Paperwhite 3", 300ppi, codename **PW3 / "wario"** in the
  jailbreak community's device naming.
- **Firmware at start**: 5.13.7
- **Architecture**: armel (soft-float ARM) — this matters a lot; see below.

## Picking a Jailbreak Method

The jailbreak landscape for older Kindles is a patchwork of firmware-version-gated
exploits, each covering a narrow range:

| Method | Firmware range | Notes |
|---|---|---|
| Popcorn | any (hardware) | Requires opening the case and soldering a wire between a component pad and a test point to force the SoC into USB recovery mode. Works regardless of firmware but is a real teardown-and-solder job. |
| KindleBreak | 5.10.3–5.13.3 | Software-only |
| WatchThis | 5.12.2.2, 5.13.4–5.14.2 | Software-only, ships exact firmware-matched payloads |
| LanguageBreak | 5.14.3–5.16.2.1.1 | Software-only, most commonly referenced online |

**First attempt: LanguageBreak.** Most current writeups point at this one, since
it's actively maintained and covers a wide firmware ceiling. Went through the full
process — demo mode entry, sideloading the exploit payload, the Chinese-language-pack
trigger trick, hotfix application — and it consistently failed with **"Update error:
007"** on the hotfix step, no matter how carefully each step was redone (verified via
SHA256 hashes that files weren't corrupted, verified firmware hadn't drifted, redid
the whole sequence twice with extra care).

Read the actual exploit script (`jb`) to see what it does on execution: the very
first things it does are `touch /mnt/us/LanguageBreakRan` and write a log file, both
on the USB-visible partition. **Neither file ever existed on the device, across every
attempt.** That's conclusive: the exploit script itself was never executing — the
underlying vulnerability trigger just wasn't firing on this specific device/firmware,
so every hotfix attempt afterward was always doomed regardless of how carefully it
was retried.

**Root cause, found afterward**: firmware 5.13.7 is actually *below*
LanguageBreak's supported floor (5.14.3+). The correct method for this exact
firmware is **WatchThis**, which — bonus — ships an *exact* firmware-matched
payload (a `PW3-5.13.7.zip` specifically, not a generic exploit spanning a firmware
range), a much better sign of fit than LanguageBreak's broader-but-wrong-floor
coverage.

## WatchThis: What Actually Worked

The WatchThis process differs from LanguageBreak in a few structural ways:

- Starts with an actual **factory reset**, with locale explicitly set to
  **en_GB / English (United Kingdom)** — not a from-normal-English demo-mode entry.
  (The setup wizard doesn't show "en_GB" as a language option directly; the region
  step appears *after* language selection, and picking English + United Kingdom
  region there is what produces the en_GB locale.)
- Exploit payload files go into a `.demo` folder created at the Kindle root
  (`.demo/PW3-5.13.7.zip` + `.demo/demo.json` + an empty `.demo/goodreads/`), not
  directly at the root like LanguageBreak.
- Trigger is **Settings → Help & User Guides → Get started**, not a
  language-selection screen trick.
- The same two-finger-tap-and-swipe gesture and `;demo`/`;uzb`/`;dsts` search-bar
  diagnostic commands carry over from the general Kindle jailbreak toolkit.

### Notable snags along the way

- **Frozen "demonstration device missing content" screen with an unresponsive
  "Configure Device" button.** This shows up whenever demo mode can't reach the
  network to sync content, and the standard "just tap the button" recovery doesn't
  always work. The actual fix: a precise gesture — **two-finger tap on the right
  edge of the white message box** (not just anywhere on screen), release both
  fingers, then a **single-finger swipe from right to left**. This is
  under-documented and easy to miss; the more commonly cited "bottom-right corner"
  gesture location is imprecise.
- **"Application error, application could not be started" exiting the demo menu.**
  Documented in WatchThis's own troubleshooting notes: hard-reboot, re-enter the
  demo menu, select Sideload Content → Done again *without* USB connected this
  time, then retry exiting.
- **The hotfix install actually succeeded** on the first real attempt — this was
  the exact step that failed three times running under LanguageBreak, which in
  hindsight was the clearest confirmation that WatchThis was the right method for
  this firmware all along.
- **Jailbreak confirmed**: after the hotfix installed and the device rebooted back
  to a normal home screen, checked the filesystem for the standard community
  jailbreak markers (`mkk`, `libkh`, `rp` folders in the root) — all present.

## KUAL + MRPI Install

Getting the app launcher (KUAL) working took longer than the jailbreak itself, for
three independent reasons stacked on top of each other:

1. **Skipped a required prerequisite.** The documented post-jailbreak sequence is
   actually *(1) install a general "Universal Hotfix", (2) then install KUAL/MRPI* —
   easy to miss and jump straight to step 2. KUAL's installer explicitly requires a
   specific hotfix to have been applied first; the jailbreak's own hotfix (which
   just gets the device out of demo mode) is a separate thing and doesn't provide
   this.

2. **Hotfix version mismatch.** After installing the Universal Hotfix, KUAL
   appeared in the library but crashed immediately on launch
   ("Application error — the selected application could not be started"). The
   installer's own documentation specifies a *maximum* supported hotfix version —
   installing the *latest* release instead of that pinned version broke launch
   compatibility specifically for armel devices (older Kindles, pre the
   hard-float architecture cutover introduced partway through the Kindle product
   line). The hotfix project's own release history around that version boundary
   has commit/release messages that are basically a live diary of this exact bug
   getting fixed — a strong signal in hindsight. Reinstalling the specific pinned
   version instead of "latest" fixed it immediately.

3. **Stale library registration.** There's a known class of bug where a factory
   reset wipes the app registry but leaves a previously-installed app's binary
   sitting in the rootfs — the launcher then sees the leftover binary, assumes the
   app is already installed, and tries to launch a registry entry that doesn't
   exist. Worth knowing about even though it turned out not to be the actual cause
   here (the version mismatch above was the real fix).

After KUAL was working, installed the OTA-blocking extension via KUAL's menu
(rename the update binaries) so the firmware stays locked at a jailbreak-compatible
version and won't silently update itself out from under the setup. Once that's
done, it's safe to turn Wi-Fi back on — the device connects normally but won't
download/install OTA updates.

## Installing the Dashboard Itself

kdashboard isn't just a KUAL app — it's a bring-your-own-backend kit: you run your
own instance of a self-hosted Postgres-backed platform for the database and edge
functions, connect your own Telegram bot, and cross-compile a native C++ binary for
the Kindle's ARM architecture.

High-level flow:
1. Clone the repo, install dependencies, log in and create/link your own backend
   project.
2. Bootstrap the schema, secrets, and edge functions.
3. Add backend secrets (base URL + an admin-level API key — see gotcha below).
4. Connect a Telegram bot (create one via BotFather, discover your chat ID,
   register a webhook).
5. Cross-compile the native KUAL package for the Kindle's ARM target.
6. Copy the built package onto the device and configure it.
7. Launch via the KUAL menu.

### Setting this up on Windows surfaced several real bugs

None of this was tested on Windows upstream, so getting a bring-your-own-backend
project like this fully working from a Windows machine turned up a handful of
genuine, fixable bugs — in the setup tooling, in the native C++ source, and in one
case in a third-party CLI dependency. All of these got written up in detail and
contributed back upstream as a pull request against the dashboard project, so
they're documented there rather than duplicated here. Summary of what was found:

- The setup scripts' calls out to the backend CLI didn't work at all on Windows —
  three different, stacked failure modes depending on how the child-process call
  was structured (missing-shell resolution failures, a Node.js security hardening
  change that blocks a naive fix, and silent argument corruption once you work
  around that). Fixed by invoking the CLI's own JS entry point directly instead of
  going through a shell at all.
- The backend CLI itself has a native crash bug on Windows when importing one
  particular SQL migration file — worked around by using a different (but
  equivalent) CLI subcommand.
- The native C++ dashboard source used a POSIX-only math constant that isn't
  guaranteed by the C++ standard, breaking the cross-compile.
- **The big one**: after getting a clean build onto the device, the dashboard would
  render once and then the screen would go completely unresponsive — touch input
  got grabbed and never released. Root-caused via targeted logging (not just
  reading the code) to a specific system call used to flush a memory-mapped
  framebuffer to the display, which hangs indefinitely on this particular Kindle's
  display driver. It's meant for syncing *file*-backed memory mappings to disk and
  isn't actually needed for a *device*-backed framebuffer mapping at all — removing
  it fixed rendering entirely. The exact same bug turned out to be duplicated in a
  second function (the tap visual-feedback effect), which is why the very first tap
  in any session would work but every tap after that would silently do nothing.
- Dark mode had no visible effect — it only inverted specific embedded images, never
  the actual screen background/text. Added a proper full-canvas inversion pass.
- The repo had no `.gitattributes`, so any Windows contributor with the common
  `core.autocrlf=true` git setting would silently get Windows-style line endings
  checked out into shell scripts meant to run on the Kindle — which then fail
  completely silently on-device (no error, no log output at all), very easy to
  misdiagnose as a filesystem permissions problem instead of what it actually is.

### A gotcha worth flagging explicitly: API key confusion

The dashboard's backend functions need an **admin/service-level** API key to write
to the database directly, server-side. It's easy to accidentally grab the wrong
kind of key — a personal account-level key from the web dashboard, or the public
anonymous key auto-written to a local env file — either of which produces a generic
"invalid token" error with no indication of *which* key is wrong. The correct key
is the project-scoped admin key that the CLI tool itself uses internally (found in
its own local project config file after linking a project), not anything you'd
naturally reach for from the web dashboard UI.

## General Gotchas / Lessons

- **USB Drive Mode shows any time the Kindle is plugged in** — it does *not* mean
  the device is in some special file-staging mode. Don't assume anything about
  device state just because a drive letter appeared.
- **The Kindle's search bar accepts special diagnostic commands** even though the
  UI presents it as a book/store search box — this is normal, long-standing Kindle
  jailbreak-community behavior, not a bug or a coincidence.
- **Airplane Mode needs to stay on throughout the jailbreak process** to prevent an
  OTA update from silently patching out the vulnerability you're relying on. Only
  turn it back on after OTA updates have been explicitly disabled.
- When a documented process specifies a hotfix/tool *maximum* version, actually use
  that version — grabbing "latest" by default is a reasonable instinct that broke
  things twice in this project alone.
- If something fails identically no matter how carefully you retry the same steps,
  stop retrying and go read the actual script/binary being invoked. In both major
  blockers here (the jailbreak exploit never firing, and the dashboard render hang),
  the fix only came from checking what the code *actually* does at the point of
  failure rather than continuing to redo the documented process more carefully.

## Credits / Tools Used

- [LanguageBreak](https://github.com/notmarek/LanguageBreak)
- [WatchThis](https://kindlemodding.org/jailbreaking/Legacy/WatchThis/) (via
  [kindlemodding.org](https://kindlemodding.org))
- [PEKI](https://github.com/KindleTweaks/PEKI) (KUAL installer)
- [MRInstaller / MRPI](https://kindlemodding.org)
- [Universal Hotfix](https://github.com/KindleModding/Hotfix)
- [kdashboard](https://github.com/thecodedose/kdashboard)
- [Zig](https://ziglang.org) (used for cross-compiling the native ARM binary from
  Windows, no separate toolchain needed)
