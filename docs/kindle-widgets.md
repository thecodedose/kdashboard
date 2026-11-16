# Kindle dashboard widgets

The native Kindle home screen has a Quick Note card and direct Narwal X10 Pro
controls. Tapping
Quick Note opens the complete message with previous/next page controls.

## Vacuum controls

The home card contains a white X10 Pro technical-outline illustration, live
robot state, and battery. It has one tap target and no appliance controls.
Tapping it opens the full vacuum page with the robot's actual floor map,
room names, room selection, live status, and Start/Pause/Resume/Dock controls.

Choose Whole Home or tap rooms on the map (or in the Rooms list). Four direct
mode buttons on the map and settings pages select Vacuum, Mop, Vacuum then mop,
or Vacuum and mop; modes do not require cycling through a generic Cleaning row.
The start button names the selected mode (for example, Start Vacuum / Start Mop).
Vacuum Settings and Mop Settings are separate tabs: suction is in Vacuum,
water and scrub are in Mop, and both expose total passes and coverage for the
selected clean. Options include four suction levels;
three water levels; normal/high mop scrub; one to three passes; and
standard/meticulous coverage. Settings are a draft until Start Cleaning is
pressed. The selected settings apply uniformly to the chosen rooms.
The request reads the current map ID immediately before starting; an empty
room selection or changed map blocks the clean instead of falling back to
whole-home cleaning. Vacuum-only / normal suction / one pass is the default.

Map pixels and room labels come from the local robot protocol. Zlib or raw
Deflate grids are decoded by the bundled MIT-licensed Miniz inflater with
bounded input/output and pixel-count checks. The dock is marked D. A live
robot marker and progress are shown only when the robot supplies valid
position/progress broadcasts; neither is inferred from idle state.
The native page provides the core cleaning workflow, not app-only account,
schedule, firmware, or map-editing features.

Set `NARWAL_HOST` and `NARWAL_IDENTITY_FILE` in the device-local dashboard
`config.sh`. Obtain the identity with the [connection test](narwal-direct-connection.md).
Keep the identity and any floor maps private. The launchers also support the
standalone connection-test configuration as a fallback.

All communication is directly over the LAN. A single worker serializes status
queries and commands; touch and rendering never block on the network. Status polls
run every 30 seconds while idle and every five seconds while cleaning or
returning, and after a command. The full page refreshes map data every 30
seconds, and briefly subscribes to optional live telemetry. Unchanged status does not repaint e-ink.
Controls are disabled when offline, busy, or in an incompatible robot state.
An accepted Start remains guarded while the robot transitions into cleaning;
an uncertain command is not retried automatically. Accepted commands are
shown as accepted until subsequent telemetry confirms the actual robot state.

Physical actions require a touch in the original calibrated coordinates;
legacy mirrored-coordinate navigation retries cannot dispatch vacuum commands.
The standalone status probe remains read-only.

`make -C kindle/native check` checks the
WebSocket transport, status and map parsing, vacuum-only task encoding, all cleaning modes against a mock robot, refusal/timeout handling, bounded map
inflation, room/map selection guards, duplicate taps, and
native layout/touch behavior at both Kindle resolutions.

The existing Calendar API connection and explicit Calendar page remain available.

## Update the quick note

The singleton is `public.dashboard_notes`, with `id='quick_note'`. An authorized assistant or admin tool can
update its `text` through your own InsForge admin connection. For example:

```sql
UPDATE public.dashboard_notes
SET text = 'Remember the donation bag.'
WHERE id = 'quick_note';
```

Use an empty string to clear it. The maximum note size is 16,000 UTF-8 bytes.
Paragraphs and long words are preserved and paginated. Text uses the native
app's existing bitmap font. The note remains until replaced; it is not tied
to a particular date. A database trigger updates `updated_at`, and the live
event stream observes only that timestamp to notify the Kindle of a change.

Alternatively, `PUT /functions/kindle-dashboard-widgets` accepts
`{"text":"Your note"}` with the existing `x-dashboard-toggle-token` header.

## Google Calendar connection

Enable Google Calendar API in your own Google Cloud project. The helper uses
the Web application client and loopback callback described in [Google Health](google-health.md).
Save that client at `.google-health/client-secret.json`, or pass its path to
`calendar:connect`. No Health authorization is needed to connect Calendar.

```sh
npm run calendar:connect
npm run calendar:configure
```

The helper requests only
`https://www.googleapis.com/auth/calendar.events.readonly`. Calendar
credentials live in Git-ignored `.google-calendar/connection.json` with
owner-only permissions. The Google Health connection file is left unchanged.
The server secrets are `GOOGLE_CALENDAR_CLIENT_ID`,
`GOOGLE_CALENDAR_CLIENT_SECRET`, `GOOGLE_CALENDAR_REFRESH_TOKEN`, and
`GOOGLE_CALENDAR_ID` (default `primary`). Review your OAuth app's publishing state and reconnect when its authorization
expires. No account authorization or Google Cloud project is bundled with this kit.

Calendar is read whenever an authorized Kindle dashboard refreshes; it does
not create or change events. The selected day uses `DASHBOARD_TIMEZONE` (UTC by default), including daylight-saving changes. The
Google API expands recurring events, includes overlapping overnight events,
and omits cancelled events. The agenda supports up to 100 events per day.
An authorization or upstream error shows Calendar unavailable while leaving
the quick note usable. API reference:
[events.list](https://developers.google.com/workspace/calendar/api/v3/reference/events/list).

## Backend and installation

`npm run kit:backend` applies the notes schema and deploys the widget function
alongside the core dashboard functions.

RLS is enabled and anonymous/authenticated table privileges are revoked.
`kindle-dashboard-widgets` requires the device token for all reads and writes.
`kindle-dashboard-data` adds `quick_note` and `calendar` only when that token
is supplied. Dashboard JSON requires the read token, and private widget contents require
the toggle token as well. SSE sends only a change hash.
The native app sends both configured tokens.

Build and install with the existing native workflow:

```sh
make -C kindle/native extension-zig ZIG=/path/to/zig
npm run native:install
```

Installation requires the Kindle mounted by USB. Preserve its existing
`config.sh` and `DASHBOARD_TOGGLE_TOKEN`. These widgets use native Kindle views.

## Validation

`npm run check` includes backend widget permission, UTF-8 size, Calendar
pagination/error tests and native long-note/touch-navigation checks at both
Kindle resolutions. PNG previews under `kindle/native/build/widgets-preview/`
are local renders; sample events and notes are not written to the account.
