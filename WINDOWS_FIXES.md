
# Windows Setup + Native Build Fixes

Notes from getting this project fully working end-to-end on a Windows host, for a
Kindle Paperwhite 3 (armel/soft-float, firmware 5.13.7), intended as background for
a PR back to the upstream repo. Every problem found, its root cause, and the fix
applied — grouped by area. File paths are relative to the repo root.

This file covers **kdashboard repo code/script bugs only** — things that belong in
a PR against this repo. The full end-to-end journey (jailbreaking the Kindle itself
with LanguageBreak/WatchThis, KUAL/MRPI install issues, InsForge account setup) is
documented separately in `../README.md` at the root of this checkout, since none of
that is upstream-repo-relevant except where it exposed an actual bug here. Two
things from that broader journey are worth cross-referencing because they're really
about *this* project, not the jailbreak itself:

- **InsForge API key confusion** (see `../README.md`, Phase 3 progress log): the
  `INSFORGE_API_KEY` the `telegram-webhook`/`health-sync` functions need via
  `createAdminClient()` is a project-scoped **admin/service key** (`ik_`-prefixed,
  found in the CLI's own `.insforge/project.json` after `create`/`link`) — not the
  personal `uak_`-prefixed key from the InsForge web dashboard, nor the
  `anon_`-prefixed public key written to `.env.local`. All three look superficially
  similar and are easy to mix up; using the wrong one produces a generic
  `{"error":"Function execution failed","message":"Invalid token"}` 500 with no
  hint about *which* key is wrong. Worth a callout in `docs/INSTALL_FOR_USERS.md`.
- **`kit:backend`'s `db import` reliability** — see item #2 below, found while
  running the documented `npm run kit:backend` setup step from
  `docs/INSTALL_FOR_USERS.md`.

## 1. Setup scripts assume a POSIX host

`scripts/bootstrap-insforge-kit.mjs` and `scripts/configure-telegram.mjs` both shell
out to `npx @insforge/cli ...` via `execFileSync("npx", [...])`. This doesn't work on
Windows for several stacked reasons:

- **`ENOENT`**: `npx` on Windows is a `.cmd` shim, not a directly-executable file.
  `execFileSync` without `shell: true` can't find/run it.
- **`EINVAL`**: switching to `execFileSync("npx.cmd", ...)` directly (still without
  `shell: true`) fails differently — recent Node versions (the CVE-2024-27980
  hardening) refuse to spawn `.cmd`/`.bat` files without going through a shell.
- **Argument mangling with `shell: true`**: once you add `shell: true`, Node just
  concatenates `command + args.join(' ')` into a single string handed to `cmd.exe`.
  Any argument containing spaces or newlines — a `--name "Kindle Dashboard Data"`
  flag, or worse, an entire multi-line SQL migration file passed as a single CLI
  arg — gets silently split into multiple broken arguments by cmd.exe's naive
  parsing. This produced very confusing downstream errors ("too many arguments for
  'query'. Expected 1 argument but got 7", "too many arguments for 'deploy'.
  Expected 1 argument but got 3").

**Fix**: install `@insforge/cli` as a real local dependency
(`npm install --no-save @insforge/cli`) and invoke its JS entry point directly via
`execFileSync(process.execPath, [cliEntryPath, ...args])`. This is `node.exe` calling
a `.js` file with a plain argument array — no shell, no `.cmd` resolution, no
argument-string reassembly, and it's fully cross-platform. See the `run()` helper in
both scripts.

## 2. `db import` crashes natively on Windows

`applyMigration()` originally used `db import <path>`. On the recipes migration
specifically (`migrations/20260629052000_create-recipes.sql`), the CLI printed
`Error: INVALID_INPUT` immediately followed by a native crash:

```
Assertion failed: !(handle->flags & UV_HANDLE_CLOSING), file src\win\async.c, line 94
```

That's a libuv-level assertion failure inside the CLI's own bundled Node runtime —
a real bug in `@insforge/cli db import` on Windows, not something fixable from the
calling script. The crash occurs after the migration is at least partially applied
(the recipe/ingredient rows had already been inserted), so retrying `db import`
after the crash immediately fails again with a duplicate-key conflict, requiring a
manual cleanup query.

**Fix**: switched `applyMigration()` back to `db query <full file contents as a
single argument>` instead of `db import <path>`. This works reliably once the
shell-quoting problem above is fixed (a multi-line SQL string as one array element
passed straight to `execFileSync` is completely safe without a shell in the loop).
`db import` may well be fine on Mac/Linux; this is a Windows-specific workaround,
not a claim that `db import` is broadly broken.

## 3. `M_PI` undefined (native C++ build)

`kindle/native/src/kindle_dashboard.cpp` uses `M_PI` (for the circular progress-ring
drawing) without defining it. `M_PI` is a POSIX/glibc extension to `<math.h>`, not
part of the C++ standard — it happens to be defined on most Linux/Mac toolchains by
default, but isn't guaranteed, and isn't defined when compiling with Zig's bundled
libc++ (used for cross-compiling to the Kindle's `arm-linux-musleabi` target from a
non-Linux host). Build fails with `use of undeclared identifier 'M_PI'`.

**Fix**: added a portable guard right after `#include <math.h>`:

```cpp
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
```

## 4. `msync(..., MS_SYNC)` hangs forever on this Kindle's fbdev driver

This was the big one — found via bisection logging, not static reading. Symptom:
after a factory-fresh jailbreak + KUAL install, running the dashboard ("Refresh
Once" or "Start Dashboard") would fetch data and start rendering successfully, but
the screen would go fully unresponsive and never actually show the new content —
touch input got grabbed (`EVIOCGRAB`) and never released, so even the Kindle's own
UI stopped responding until a hard power-cycle.

The native log showed execution reaching `render=save-pgm ...` and then stopping
cold — no `render=framebuffer ok` line ever appeared, across multiple clean
reproductions.

**Debugging approach**: added `fprintf(stderr, ...); fflush(stderr);` calls around
every remaining step in `renderToFramebuffer()` after the pixel-writing loop
(pixel-loop start/done, msync start/done, eips start/done), rebuilt, redeployed,
reproduced. Output pinpointed it exactly:

```
debug=pixel-loop-done
debug=msync-start
```

`debug=msync-done` never printed. `msync(fb, screensize, MS_SYNC)` — called on the
`mmap`'d `/dev/fb0` region — blocks indefinitely on this device's framebuffer
driver.

**Root cause**: `msync` is designed to flush *file-backed* mmap pages to disk. It's
unnecessary for a *device-backed* mapping like a framebuffer — writes to a mmap'd
`/dev/fb0` take effect immediately (the "backing store" is the hardware itself, not
a file that needs syncing), and this particular fbdev driver evidently doesn't
implement the sync callback correctly, so the call just blocks forever instead of
either working or returning an error.

**Fix**: removed the `msync` call entirely from `renderToFramebuffer()`. Kept
`munmap`/`close`/the `eips ''` refresh trigger immediately after. Retested clean:
`render=framebuffer ok ... ms=750`, and the device stayed fully responsive
afterward (KUAL menu navigable again, "Stop Dashboard" reachable).

This is very likely device/driver-specific (older armel Kindles vs. newer armhf
ones may behave differently here), so worth keeping an eye on whether this
regresses anything on other hardware — but `msync` on a framebuffer mmap is
unusual/unnecessary in general, so removing it should be safe everywhere.

## 5. Dark mode only inverted embedded images, not the dashboard itself

`--invert-images` (wired up via `DASHBOARD_FORCE_INVERT_IMAGES` in
`start-dark.sh`/`once-dark.sh`) only affected the flip-decision for specific loaded
bitmap assets (`g_invert_images` gates a check inside the image-loading path). The
actual canvas — background, text, card borders — was never inverted, so "dark mode"
had no visible effect on the overall UI; it just looked identical to light mode.

**Fix**: added a full-canvas inversion pass, gated on the same `g_invert_images`
flag, applied right after `drawCurrentDashboard()` completes and before the canvas
gets written out (both in `renderToFramebuffer()` and the `dumpBitmapPreview()`
fallback path used for local/fixture rendering):

```cpp
void invertCanvasIfDark(Canvas* canvas) {
  if (!g_invert_images || !canvas->pixels) return;
  const size_t total = static_cast<size_t>(canvas->width) * static_cast<size_t>(canvas->height);
  for (size_t i = 0; i < total; i++) canvas->pixels[i] = static_cast<unsigned char>(255 - canvas->pixels[i]);
}
```

## 6. No `.gitattributes` — Windows checkouts silently get CRLF shell scripts

The repo has no `.gitattributes` file. The stored blobs for
`kindle/kual/kindle-dashboard/bin/*.sh` are LF (verified: `git diff` against origin
shows zero content difference after normalizing a local checkout back to LF), but
on a Windows machine with the common `core.autocrlf=true` git setting, `git
checkout` silently converts them to CRLF in the working tree — with no warning,
since that's exactly what `autocrlf=true` is documented to do. Whatever gets copied
from that working tree to the Kindle then has CRLF.

KUAL invokes these scripts via an explicit `/bin/sh <path>` (per `menu.json`), so
the scripts' own missing execute bit isn't the issue — `/bin/sh` (BusyBox/dash on
the Kindle) just chokes on the embedded `\r` characters. The failure is completely
silent: no error dialog, no log output at all (not even the first line of the
script, a plain `echo >> logfile`), the KUAL action just does nothing. Easy to
misdiagnose as a FAT/exFAT permissions problem (a very reasonable first guess,
since the Kindle's USB-exposed filesystem genuinely has no Unix permission bits)
but it isn't — it's purely the line-ending content, and it's entirely dependent on
the *contributor's own local git config*, not anything in the repo itself.

**Fix**: added `.gitattributes` at the repo root, pinning `*.sh` (and `*.sql`, for
the same reason — see #2) to `eol=lf` regardless of a contributor's `core.autocrlf`
setting. This is the actual fix — it means no Windows contributor hits this again,
rather than relying on everyone individually knowing to run
`sed -i 's/\r$//' kindle/kual/kindle-dashboard/bin/*.sh` after every checkout.

## 7. A second, duplicated `msync` hang — this time on every tap

After fixing #4, "Refresh Once" worked perfectly, but interactive taps in "Start
Dashboard" mode still did nothing after the first one. `pollExitTouch()` in the
touch-watcher thread kept logging `input=action touch action=N x=.. y=..`
(confirming `applyTouchWithDebounce()` was matching regions and setting
`g_pending_action` correctly), but `handlePendingTouch()`'s own per-action log
lines (`touch=open-list`, `touch=exit`, etc.) never appeared after the very first
tap — meaning the function wasn't being re-entered, not just failing silently.

Added temporary trace logging at the top of the main loop and at the entry of
`waitForWakeEvent()` (`debug=main-loop-top pending_action=.. event_refresh=..` /
`debug=wait-enter seconds=..`) to confirm `g_pending_action` really was being set
correctly by the touch thread. It was — and `handlePendingTouch()` visibly started
running each time (`showTouchVisualFeedback()`'s `visual-feedback=tap ...` line
printed every time), but execution stopped dead immediately after that, before
reaching any of the action-specific branches.

**Root cause**: the exact same bug as #4, duplicated in a second location.
`showTouchVisualFeedback()` calls `flashTouchRectOnFramebuffer()` (when a tap
region has a highlight rect configured) to draw a brief invert-flash at the tapped
location — and that function has its own independent pair of
`msync(fb, screensize, MS_SYNC)` calls on the same device-backed `/dev/fb0` mmap,
which hang indefinitely for the same reason as #4. The very first tap in a session
happened to hit a region with no flash rect configured, so it worked; every tap
after that (all the real interactive regions — list cards, exit, nav) went through
`flashTouchRectOnFramebuffer()` and hung there, permanently blocking the thread
that owns `g_running`/rendering from ever getting back to the main loop.

**Fix**: removed both `msync` calls from `flashTouchRectOnFramebuffer()`, same
reasoning as #4. Grepped the whole file afterward to confirm no other `msync` calls
remain. Removed the temporary trace logging once confirmed.

**Lesson for upstream**: `msync` isn't needed anywhere in this codebase for the
`/dev/fb0` mmap — it appears twice, both times paired with `munmap`/`close`/an
`eips` refresh call directly afterward, and neither call site needs the explicit
sync. Worth a quick search for the string `msync` if this pattern gets copied
elsewhere in the future.
