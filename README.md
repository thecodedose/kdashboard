# Kindle Planner Dashboard

Monochrome, Kindle-friendly home dashboard with Telegram updates, Quick Note,
Google Health and Calendar, direct Narwal vacuum controls, Spotify music and
private delivery summaries. Each installation brings its own backend and accounts.

See the [optional integration guides](docs/INSTALL_FOR_USERS.md#optional-integrations-in-this-version)
for setup. No personal account connections, device identities or sync automations
are bundled. `npm run check` runs the backend and native regression suite.

## Requirements

- A jailbroken Kindle with KUAL installed for the native dashboard.
- Node.js 20+ and npm for scripts, checks, and backend bootstrapping.
- An InsForge account for the database, edge functions, and secrets.
- A Telegram bot token from BotFather for chat-based planner updates.
- Optional: an OpenAI API key for natural-language message parsing. Without
  it, the webhook uses the built-in command parser.
- Optional: Zig or a Kindle-compatible ARM cross compiler to rebuild the KUAL
  package.
- Optional: Xcode and a real iPhone for the Health Sync companion app.

## Setup Instructions

This repo can be published as a bring-your-own-backend kit. Each owner runs
their own InsForge project, Telegram bot, and KUAL package config instead of
connecting to this checkout's production backend.

Start with [docs/INSTALL_FOR_USERS.md](docs/INSTALL_FOR_USERS.md).

If you are using a coding assistant for setup, use
[docs/SETUP_WITH_ASSISTANT.md](docs/SETUP_WITH_ASSISTANT.md).

## Project Structure

- `functions/`: InsForge edge functions for dashboard JSON, live events,
  item toggles, Telegram parsing/actions, version checks, and Health Sync
  uploads.
- `migrations/`: Postgres schema and optional sample-data migrations used by
  the bring-your-own-backend setup.
- `kindle/native/`: Native C++ e-ink dashboard renderer, local render check,
  and KUAL package build targets.
- `kindle/kual/kindle-dashboard/`: KUAL extension files, shell launchers,
  placeholder assets, and `config.sh.example`.
- `kindle/launch-dashboard.sh`: Kindle-side launcher used by installed
  shortcuts and native install helpers.
- `ios/HealthSyncCompanion/`: Optional iOS app that reads Apple Health daily
  aggregates and posts them to the `health-sync` function.
- `scripts/`: Setup and maintenance helpers for InsForge bootstrapping,
  Telegram webhook configuration, Kindle native install, and Kindle proof
  checks.
- `docs/`: Human and coding-assistant setup guides.

## Technical Breakdown

### The Stack

**Renderer:** Native C++ app

**Backend:** InsForge

**Inputs:** Telegram bot, optional iOS HealthKit or Google Health, Google Calendar,
Spotify, local vacuum telemetry, and owner-synced delivery summaries

Code entry points:

- Native app: [`kindle/native/src/kindle_dashboard.cpp`](kindle/native/src/kindle_dashboard.cpp)
- Backend functions: [`functions/`](functions/)
- Health companion: [`ios/HealthSyncCompanion/`](ios/HealthSyncCompanion/)
- Owner config template:
  [`kindle/kual/kindle-dashboard/config.sh.example`](kindle/kual/kindle-dashboard/config.sh.example)

### Backend Spine

InsForge handles the cloud layer:

**Postgres Database:** chores, groceries, recipes, meal plans, health summaries,
challenge logs, quick notes, and delivery summaries

**Edge functions:** read dashboard data, sync HealthKit, parse Telegram updates,
toggle Kindle tasks, SSE live events

Relevant files:

- Schema:
  [`migrations/001_planner_lists.sql`](migrations/001_planner_lists.sql),
  [`migrations/20260627000000_create-health-daily-summaries.sql`](migrations/20260627000000_create-health-daily-summaries.sql),
  [`migrations/20260629052000_create-recipes.sql`](migrations/20260629052000_create-recipes.sql),
  [`migrations/20260629083000_create-challenge-daily-logs.sql`](migrations/20260629083000_create-challenge-daily-logs.sql),
  [`migrations/20260629162000_create-meal-plan-entries.sql`](migrations/20260629162000_create-meal-plan-entries.sql)
- Dashboard read endpoint:
  [`functions/kindle-dashboard-data.ts`](functions/kindle-dashboard-data.ts)
- Toggle endpoint:
  [`functions/kindle-dashboard-toggle.ts`](functions/kindle-dashboard-toggle.ts)
- Live event endpoint:
  [`functions/kindle-dashboard-events.ts`](functions/kindle-dashboard-events.ts)
- Telegram webhook:
  [`functions/telegram-webhook.ts`](functions/telegram-webhook.ts)
- Health sync endpoint:
  [`functions/health-sync.ts`](functions/health-sync.ts)

The dashboard read endpoint builds a compact payload and hashes the visible
state into a version:

```ts
const payload = {
  ...payloadWithoutVersion,
  version: hashText(JSON.stringify({
    health: payloadWithoutVersion.health,
    challenge: payloadWithoutVersion.challenge,
    lists: payloadWithoutVersion.lists,
    meal_plan: payloadWithoutVersion.meal_plan,
    recipes: payloadWithoutVersion.recipes
  }))
};
```

### Telegram Bot

`Add milk to grocery`

Telegram sends a webhook to InsForge.

The webhook checks:
secret header + allowed chat ID

Code:
[`functions/telegram-webhook.ts`](functions/telegram-webhook.ts)

Then it parses the message into a strict action (how on next slide):

```json
{
  "kind": "planner",
  "action": "add",
  "list_key": "grocery",
  "items": ["milk"],
  "all_lists": false
}
```

The webhook gate is deliberately small:

```ts
const receivedSecret = req.headers.get("x-telegram-bot-api-secret-token");
if (receivedSecret !== configuredSecret) {
  return jsonResponse({ ok: false, error: "Unauthorized" }, 401);
}

if (chatId !== allowedChatId) {
  return jsonResponse({ ok: true, ignored: true, reason: "chat_not_allowed" });
}
```

The backend updates database and sends a confirmation back.

Examples:

`Added milk to grocery.`

### Message Parsing

**AI Parsing**

The Telegram message is sent to OpenAI with a strict instruction: convert this
message into one JSON action.

AI parsing handles flexible language.

Code:
[`parseTelegramMessage`](functions/telegram-webhook.ts) in
[`functions/telegram-webhook.ts`](functions/telegram-webhook.ts)

**Deterministic parsing**

If OpenAI is missing, fails, or returns invalid JSON, the backend uses
handwritten rules in the code.

Deterministic parsing follows hardcoded rules.

Either way, the backend validates the result before database changes.

The parser tries a fast deterministic pass, then OpenAI, then deterministic
fallback:

```ts
async function parseTelegramMessage(message: string): Promise<TelegramAction | null> {
  const fastAction = parseFastHeuristicMessage(message);
  if (fastAction) return fastAction;

  const openAiKey = Deno.env.get("OPENAI_API_KEY");
  if (!openAiKey) {
    return parseMessageHeuristically(message);
  }

  // OpenAI response is parsed and validated before any database write.
}
```

List names also have hardcoded aliases:

```ts
const LIST_ALIASES = {
  grocery: ["grocery", "groceries", "shopping", "market"],
  workout: ["workout", "exercise", "training", "gym"],
  meal: ["meal", "meals", "menu", "food"],
  todo: ["todo", "to-do", "task", "tasks", "errand", "errands"]
};
```

### iOS Companion App

The iOS app reads data from Apple Health:
steps, calories, protein, carbs, and fat.

It sends summarized day-level rows to an InsForge endpoint.

That endpoint runs an edge function which saves the daily summary to the
database.

Relevant files:

- HealthKit reads:
  [`ios/HealthSyncCompanion/HealthSyncCompanion/HealthKitSyncService.swift`](ios/HealthSyncCompanion/HealthSyncCompanion/HealthKitSyncService.swift)
- Upload client:
  [`ios/HealthSyncCompanion/HealthSyncCompanion/HealthSyncClient.swift`](ios/HealthSyncCompanion/HealthSyncCompanion/HealthSyncClient.swift)
- Backend endpoint:
  [`functions/health-sync.ts`](functions/health-sync.ts)

The iOS app collects cumulative daily sums:

```swift
async let steps = dailySums(identifier: .stepCount, unit: .count(), startDate: startDate, endDate: endDate)
async let energy = dailySums(identifier: .dietaryEnergyConsumed, unit: .kilocalorie(), startDate: startDate, endDate: endDate)
async let protein = dailySums(identifier: .dietaryProtein, unit: .gram(), startDate: startDate, endDate: endDate)
async let carbs = dailySums(identifier: .dietaryCarbohydrates, unit: .gram(), startDate: startDate, endDate: endDate)
async let fat = dailySums(identifier: .dietaryFatTotal, unit: .gram(), startDate: startDate, endDate: endDate)
```

Then it posts JSON with the sync token:

```swift
request.httpMethod = "POST"
request.setValue("application/json", forHTTPHeaderField: "Content-Type")
request.setValue(try AppConfig.healthSyncToken, forHTTPHeaderField: "x-health-sync-token")
request.httpBody = try JSONEncoder().encode(payload)
```

### Kindle Dashboard Update

The Kindle fetches one compact JSON payload from an InsForge data endpoint.

It renders the dashboard locally and caches the payload for offline use.

Relevant files:

- Endpoint:
  [`functions/kindle-dashboard-data.ts`](functions/kindle-dashboard-data.ts)
- Native fetch/cache/render loop:
  [`kindle/native/src/kindle_dashboard.cpp`](kindle/native/src/kindle_dashboard.cpp)
- KUAL launcher:
  [`kindle/kual/kindle-dashboard/bin/dashboard.sh`](kindle/kual/kindle-dashboard/bin/dashboard.sh)

Example response:

```json
{
  "ok": true,
  "generated_at": "2026-07-09T12:00:00.000Z",
  "health": {
    "steps": 8420,
    "calories": 1830,
    "protein_g": 92,
    "steps_target": 10000,
    "calories_target": 2200
  },
  "challenge": {
    "day": 12,
    "water_l": 2.5,
    "sleep_hours": 7.5,
    "workouts": 1
  },
  "lists": [
    {
      "key": "grocery",
      "title": "Grocery",
      "items": [
        { "id": "item-id", "text": "milk", "done": false }
      ]
    }
  ],
  "meal_plan": [],
  "recipes": [],
  "version": "a13f9c"
}
```

The native app fetches to a cache file first, then renders the cache:

```cpp
const int fetched = fetchToCache(dashboard_url, options.read_token, options.cache);
renderCachedPayload(&options, fetched ? "live" : "cached/offline");
```

### Native Rendering

The Kindle runs a native C++ renderer.

It draws text, boxes, progress bars, and images onto a monochrome canvas sized
for the e-ink display.

Then it writes those pixels directly to the Kindle framebuffer.

Images are stored as lightweight PGM (Portable Gray Map) assets.

Code:
[`kindle/native/src/kindle_dashboard.cpp`](kindle/native/src/kindle_dashboard.cpp)

The renderer parses JSON into a fixed dashboard struct:

```cpp
extractString(json, NULL, "version", dashboard->version, sizeof(dashboard->version), "");
dashboard->steps = extractInt(json, NULL, "steps", 0);
dashboard->calories = extractInt(json, NULL, "calories", 0);
dashboard->protein_g = extractInt(json, NULL, "protein_g", 0);
```

Then it draws into a canvas and writes to `/dev/fb0` when available:

```cpp
int fd = open("/dev/fb0", O_RDWR);
unsigned char* fb = static_cast<unsigned char*>(
  mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
);
```

### Interactivity

There is no button framework.

Touch is manual:
tap coordinates -> rectangle -> dashboard action

Each tappable area is registered as a region, so a tap on a list item can
become: mark task done, open recipe, switch day.

Code:
[`handlePendingTouch`](kindle/native/src/kindle_dashboard.cpp) and
[`postToggleItemAsync`](kindle/native/src/kindle_dashboard.cpp) in
[`kindle/native/src/kindle_dashboard.cpp`](kindle/native/src/kindle_dashboard.cpp)

Task toggles update the offline cache optimistically and post to InsForge:

```cpp
if (action == kTouchToggleItem) {
  const int next_done = g_pending_item_done ? 0 : 1;
  patchCachedItemDone(options->cache, g_pending_item_id, next_done);
  postToggleItemAsync(options->toggle_url, options->toggle_token, g_pending_item_id, next_done);
  return 1;
}
```

The matching backend endpoint updates `planner_items`:

```ts
await admin.database
  .from("planner_items")
  .update({ done: body.done, updated_at: new Date().toISOString() })
  .eq("id", id);
```

### Change Detection

An InsForge SSE edge function checks for dashboard changes.

Every few seconds, it computes a version from the dashboard data.

Same data = same version.
Changed data = new version.

Code:
[`functions/kindle-dashboard-events.ts`](functions/kindle-dashboard-events.ts)

```ts
const data = await loadDashboardData();
const version = getDashboardVersion(data);
if (!force && version === lastVersion) {
  controller.enqueue(encoder.encode(`: heartbeat ${new Date().toISOString()}\n\n`));
  return;
}
```

### Live Events

When the version changes, InsForge emits a small SSE event.

The event does not contain the whole dashboard.

It only tells the Kindle: fetch the latest JSON payload.

The event body is just the version string:

```ts
lastVersion = version;
controller.enqueue(encoder.encode(`event: planner\n`));
controller.enqueue(encoder.encode(`data: ${version}\n\n`));
```

The native watcher listens with `curl` and flips a refresh flag:

```cpp
if (strncmp(line_buffer, "event: planner", 14) == 0) {
  g_event_refresh = 1;
}
```

### Re-rendering

The Kindle treats the SSE event as a refresh signal.

It fetches the latest JSON payload, saves it to the offline cache, and
re-renders the dashboard.

If the network fails, it keeps rendering from the cached payload.

Code:
[`kindle/native/src/kindle_dashboard.cpp`](kindle/native/src/kindle_dashboard.cpp)

```cpp
if (g_manual_fetch_refresh) {
  g_manual_fetch_refresh = 0;
}

const int fetched = fetchToCache(dashboard_url, options.read_token, options.cache);
if (!renderCachedPayload(&options, fetched ? "live" : "cached/offline")) {
  addCardText(lines, &count, " Dashboard unavailable");
  addCardText(lines, &count, " Check Wi-Fi or refresh later");
  renderToEips(lines, count);
}
```
