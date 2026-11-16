# Kindle Dashboard Bring-Your-Own-Backend Install

This guide is for Kindle owners who want their own private dashboard instance.
You will run your own InsForge backend, Telegram bot, and Kindle KUAL package.

If you want a coding assistant to walk through this with you, start with
`docs/SETUP_WITH_ASSISTANT.md`.

## What You Need

- A jailbroken Kindle with KUAL installed.
- A Mac/Linux machine with Node.js 20+ and npm.
- An InsForge account.
- A Telegram bot token from BotFather.
- Optional: Zig or an ARM Kindle cross compiler if you want to rebuild the native binary yourself.

The Kindle package never stores the InsForge admin API key. It reads from token-protected
dashboard endpoints and sends item toggles through the deployed toggle function.

## 1. Create Your Backend

Clone the repo, install dependencies, and log in to InsForge:

```sh
npm install
npx -y @insforge/cli login
```

Create a fresh InsForge project, or link this checkout to an existing empty one:

```sh
npx -y @insforge/cli create --name kindle-dashboard --region us-east --template empty
```

Bootstrap the schema, generated secrets, and functions:

```sh
npm run kit:backend
```

The bootstrap script applies the public schema migrations, creates generated
`TELEGRAM_WEBHOOK_SECRET`, `HEALTH_SYNC_TOKEN`, `DASHBOARD_READ_TOKEN`, and
`DASHBOARD_TOGGLE_TOKEN` values if missing, and deploys the dashboard functions.

It skips optional sample recipe/photo migrations by default. To include the
sample-data migrations anyway:

```sh
npm run kit:backend -- --with-sample-data
```

## 2. Add Required Backend Secrets

Set the backend URL and API key for the functions. These are server-side
function secrets, not Kindle-side values.

```sh
npx -y @insforge/cli secrets add INSFORGE_BASE_URL https://your-project.insforge.app
npx -y @insforge/cli secrets add INSFORGE_API_KEY your-server-only-api-key
```

Optional natural-language parsing:

```sh
npx -y @insforge/cli secrets add OPENAI_API_KEY your-openai-key
npx -y @insforge/cli secrets add OPENAI_MODEL gpt-4o-mini
```

If `OPENAI_API_KEY` is missing, the Telegram webhook uses its built-in command
parser.

## 3. Connect Telegram

Create a bot with BotFather and send it one message. Then discover your chat ID:

```sh
npm run telegram:chat-id -- --bot-token 123456789:telegram-bot-token
```

Register the webhook and store your bot token/chat allowlist:

```sh
npm run telegram:configure -- \
  --bot-token 123456789:telegram-bot-token \
  --chat-id 123456789 \
  --webhook-url https://your-project.insforge.app/functions/telegram-webhook
```

Try a Telegram command:

```text
add milk and eggs to groceries
add clean desk to todo
mark milk done
```

## Supported Telegram Messages

The webhook supports these message types. If `OPENAI_API_KEY` is configured, it
can understand more natural phrasing; if not, the built-in parser supports the
patterns below.

### Planner Lists

Supported lists:

- Grocery: `grocery`, `groceries`, `shopping`, `market`
- Workout: `workout`, `exercise`, `training`, `gym`
- Meal notes: `meal`, `meals`, `menu`, `food`
- Todo/chores: `todo`, `to-do`, `task`, `tasks`, `errand`, `errands`

Add items:

```text
add milk and eggs to groceries
need apples, yogurt, oats in grocery
put leg day on workout
add clean desk to todo
```

Mark items done:

```text
mark milk done
complete leg day in workout
check off clean desk from todo
```

Mark items open again:

```text
undo milk
uncheck leg day in workout
mark clean desk not done
```

Remove items:

```text
remove eggs from groceries
delete leg day from workout
drop clean desk from todo
```

Clear a list:

```text
clear todo
empty groceries
reset workout
```

If a done/open/remove command does not name a list, the webhook searches across
lists for matching item text.

### Health Targets

Set dashboard targets for steps or calories:

```text
set steps target to 12000
update calorie goal to 2100
change calories target = 1900
```

### 75 Day Challenge Check-Ins

Log water:

```text
drank 1L water
water 750ml
XL water
```

Log sleep:

```text
slept 7.5 hours
sleep 8h
```

Log a workout:

```text
workout done
completed gym
did exercise
```

### Today's Meal Plan

Meal plan commands match saved recipe titles by partial title. Use these after
you have recipes saved in the backend.

Set or replace today's meal plan:

```text
set meal plan to Sample Breakfast Bowl and Sample Toast
plan meals Sample Breakfast Bowl, Sample Smoothie
meals today: Sample Toast + Sample Smoothie
```

Add another saved recipe to today's meal plan:

```text
add meal Sample Smoothie
include Sample Toast in meals
put Sample Breakfast Bowl on meal plan
```

Clear today's meal plan:

```text
clear meal plan
reset meals today
remove meals
```

### Saved Recipes

Create or update a saved recipe. Calories, carbs, fat, and protein are required
for the built-in parser; rating and ingredients are optional.

```text
add recipe Sample Wrap calories 420 carbs 45 fat 14 protein 28 rating 4 ingredients tortilla 1 piece, beans 100 g instructions roll and toast
save meal Sample Smoothie kcal 180 c 28 f 4 p 6 rating 4.5
create recipe Sample Bowl cal 360 carbs 48 fat 8 protein 24 ingredients yogurt 200 g, oats 20 g
```

Rate an existing recipe:

```text
rate Sample Wrap 4.5/5
set rating for Sample Smoothie to 4
give Sample Bowl a rating of 5 stars
```

## 4. Configure the Kindle Package

Build or download `kindle-dashboard-kual.tar.gz`.

Run the local syntax/render check before packaging:

```sh
npm run native:check
```

If you have Zig installed, build a soft-float ARM KUAL package:

```sh
make -C kindle/native extension-zig ZIG=/path/to/zig
```

If you have a dedicated Kindle ARM compiler, build with:

```sh
make -C kindle/native extension
```

The GNU build expects `arm-linux-gnueabi-g++` by default. Override with
`KINDLE_CXX=/path/to/compiler` if your toolchain uses a different binary name.

The package is written to:

```text
kindle/native/build/kindle-dashboard-kual.tar.gz
```

Copy it to the Kindle and extract it into the KUAL extensions directory:

```sh
tar -C /mnt/us/extensions -xzf kindle-dashboard-kual.tar.gz
```

Copy the example config and edit the URLs:

```sh
cd /mnt/us/extensions/kindle-dashboard
cp config.sh.example config.sh
```

`config.sh` should look like this:

```sh
DASHBOARD_DATA_URL="https://your-project.insforge.app/functions/kindle-dashboard-data"
DASHBOARD_EVENTS_URL="https://your-project.function2.insforge.app/kindle-dashboard-events"
DASHBOARD_TOGGLE_URL="https://your-project.insforge.app/functions/kindle-dashboard-toggle"
DASHBOARD_READ_TOKEN="replace-with-your-generated-read-token"
DASHBOARD_TOGGLE_TOKEN="replace-with-your-generated-toggle-token"
INTERVAL="3600"
DASHBOARD_KEEP_AWAKE="1"
DASHBOARD_SLEEP_WINDOW="off"
INVERT_IMAGES="0"
```

Fetch the tokens from InsForge and paste them into `config.sh`:

```sh
npx -y @insforge/cli secrets get DASHBOARD_READ_TOKEN --json
npx -y @insforge/cli secrets get DASHBOARD_TOGGLE_TOKEN --json
```

Use the direct `function2.insforge.app` host for the events URL. InsForge's
regular `/functions/...` gateway can buffer SSE responses.

If you use the local installer script, set your data URL explicitly so it does
not seed from the wrong backend:

```sh
DASHBOARD_DATA_URL=https://your-project.insforge.app/functions/kindle-dashboard-data DASHBOARD_READ_TOKEN=<read-token> npm run native:install
```

## 5. Launch On Kindle

On the Kindle, open KUAL:

- `Kindle Dashboard -> Refresh Once (Light)` fetches and renders one update for Kindle light mode.
- `Kindle Dashboard -> Refresh Once (Dark)` fetches and renders one update for Kindle dark mode.
- `Kindle Dashboard -> Start Dashboard (Light)` starts hourly refresh for Kindle light mode.
- `Kindle Dashboard -> Start Dashboard (Dark)` starts hourly refresh for Kindle dark mode.
- `Kindle Dashboard -> Stop Dashboard` stops the process.

Useful Kindle-side files:

```text
/mnt/us/documents/kindle-dashboard-native.log
/mnt/us/documents/kindle-dashboard-diagnose.log
/mnt/us/documents/kindle-dashboard-data.json
```

The native app uses an always-on profile: hourly auto-refresh, manual KUAL
refresh on demand, and no overnight quiet mode by default. For optional
auto-open notes, see `kindle/README.md`.

## 6. Optional Health Sync Companion

The native iOS companion app lives in:

```text
ios/HealthSyncCompanion
```

It reads Apple Health daily steps and nutrition totals, then sends daily
aggregate rows to the `health-sync` InsForge function.

Before running the iOS app, make sure the health pieces are deployed:

```sh
npx -y @insforge/cli db migrations up 20260627000000_create-health-daily-summaries.sql
npx -y @insforge/cli secrets add HEALTH_SYNC_TOKEN <long-random-health-sync-token>
npx -y @insforge/cli functions deploy health-sync --file functions/health-sync.ts --name "Health Sync"
```

Then configure the local iOS secret:

```sh
cd ios/HealthSyncCompanion
cp Config/LocalConfig.example.xcconfig Config/LocalConfig.xcconfig
```

Edit `Config/LocalConfig.xcconfig` so `HEALTH_SYNC_TOKEN` matches the InsForge
secret. Open `HealthSyncCompanion.xcodeproj` in Xcode and run it on a real
iPhone; HealthKit data is not meaningfully testable in the simulator.

## Updating Later

When you pull a new version:

```sh
npm install
npm run kit:backend
make -C kindle/native extension-zig ZIG=/path/to/zig
```

Then replace the installed KUAL extension files, keeping your local `config.sh`.

## Privacy Notes

- Do not share `INSFORGE_API_KEY`, Telegram bot token, webhook secret, or health
  sync token.
- Treat `DASHBOARD_READ_TOKEN` and `DASHBOARD_TOGGLE_TOKEN` as device secrets.
  The read token exposes dashboard data, while the toggle token can change
  checklist state.
- The Kindle reads dashboard data through your deployed function URLs using
  the read token.
- This kit is single-owner by design. For a hosted multi-user service, every
  table and function would need per-user scoping and device pairing.

## Optional integrations in this version

The backend bootstrap now includes Quick Note, Google Health, Calendar, Music,
and Orders schemas/functions. It does not connect any external accounts or
create sync schedules. Configure only the integrations you want:

- [Google Health](google-health.md): Fitbit daily steps and nutrition.
- [Widgets](kindle-widgets.md): Quick Note, read-only Google Calendar and direct
  Narwal X10 Pro LAN controls, including room selection and cleaning modes.
- [Music](kindle-music.md): Spotify playlists/controls and optional direct playback.
- [Orders](kindle-orders.md): private delivery summaries from an owner-configured sync.

Set the backend `DASHBOARD_TIMEZONE` secret to your IANA zone, and use the same
`DASHBOARD_TIMEZONE` in Kindle `config.sh`. New integrations default to UTC.
The Health configuration helper accepts `GOOGLE_HEALTH_TIMEZONE` in its process
environment. Keep it aligned with the dashboard zone.

Xcode signing is deliberately blank. Choose your own signing team and bundle ID
before installing the optional iOS companion; update its background-task identifier
in both `Info.plist` and `AppDelegate.swift` together if you change that ID.
