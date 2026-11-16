# Google Health: Fitbit steps and nutrition

The dashboard can read Fitbit steps and food logs directly from Google Health,
without an Apple Health bridge or the iPhone Health Sync companion. This is a
single-owner integration for the currently linked InsForge project.

Each installation supplies its own Google OAuth client and authorizes its own
account. No existing account connection or scheduled job is included. The optional
`kindle-dashboard-info` function provides generic OAuth information pages on your
own backend; review them for your deployment before using them on a consent screen.

## 1. Create a Google OAuth client

1. In [Google Cloud Console](https://console.cloud.google.com/), create a project
   for your dashboard (or select your own existing project).
2. Under **APIs & Services → Library**, enable **Google Health API**.
3. Open **Google Auth Platform** and configure the app with your contact email
   and an **External** audience. While its publishing status is **Testing**, add
   the Google account used by your Fitbit under **Audience → Test users**.
4. Under **Data Access**, add these two read-only scopes:

   ```text
   https://www.googleapis.com/auth/googlehealth.activity_and_fitness.readonly
   https://www.googleapis.com/auth/googlehealth.nutrition.readonly
   ```

5. Under **Clients**, create an OAuth client with type **Web application**.
   Add this exact **Authorized redirect URI**:

   ```text
   http://127.0.0.1:8787/oauth/google-health/callback
   ```

6. Download the client JSON. Keep it local; do not paste it into chat or commit
   it. The default path used by the helper is
   `.google-health/client-secret.json`, and `.google-health/` is Git-ignored.
   You can also pass the downloaded file's path directly in the next step.

Google's [OAuth/API setup walkthrough](https://developers.google.com/health/codelabs/make-your-first-api-call)
shows the corresponding console screens.

## 2. Authorize the Fitbit account

From the repository root, using Node 20 or newer:

```bash
npm run health:connect -- /absolute/path/to/downloaded-client.json
```

The helper opens Google sign-in on your computer, validates the callback with state
and PKCE, and requests only activity and nutrition read access. Select the
Google account used in the Google Health app and allow **both** permissions.
It saves the client credentials and refresh token in
`.google-health/connection.json`, with owner-only file permissions. Tokens are
never printed. The callback server listens only on `127.0.0.1` and closes after
the response or after five minutes.

Google OAuth apps with an External audience in **Testing** receive refresh
tokens that expire after seven days for these scopes. The helper reports an
expiry when Google supplies one. For
an ongoing unattended sync, review Google's publishing requirements for your
personal app; otherwise rerun `health:connect` and `health:configure` when the
authorization expires. If you change the requested scopes, reconnect as well.
See [Google's token expiration rules](https://developers.google.com/identity/protocols/oauth2/#expiration).

## 3. Configure and deploy the backend

Verify that the CLI is linked to the intended project:

```bash
npx -y @insforge/cli current
```

Confirm it reports your own backend before uploading any credentials.

Run:

```bash
npm run health:configure
```

This uploads `GOOGLE_HEALTH_CLIENT_ID`, `GOOGLE_HEALTH_CLIENT_SECRET`,
`GOOGLE_HEALTH_REFRESH_TOKEN`, and `GOOGLE_HEALTH_TIMEZONE` as
server-only InsForge secrets. It does not deploy or enable a schedule. The
existing `HEALTH_SYNC_TOKEN`, `INSFORGE_BASE_URL`, and `INSFORGE_API_KEY` must
already be configured in InsForge. Set `GOOGLE_HEALTH_TIMEZONE` in the helper
environment to your IANA timezone; its default is UTC. Set the backend
`DASHBOARD_TIMEZONE` to the same zone.

After reviewing the relevant local function changes, deploy:

```bash
npx -y @insforge/cli functions deploy google-health-sync --file functions/google-health-sync.ts --name "Google Health Sync"
npx -y @insforge/cli functions deploy kindle-dashboard-data --file functions/kindle-dashboard-data.ts
npx -y @insforge/cli functions deploy kindle-dashboard-events --file functions/kindle-dashboard-events.ts
```

There is no new database migration. Google rows use `source=google_health` in
`health_daily_summaries`, whose existing `(date,source)` constraint supports
atomic upserts. Apple rows remain separate. Once Google credentials are
configured, the dashboard no longer falls back to Apple rows if a Google row
is missing for a date.

## 4. Verify a real sync before scheduling

Sync your Fitbit in the Google Health iPhone app first. Ensure `.env.local`
contains your existing `INSFORGE_BASE_URL` and `HEALTH_SYNC_TOKEN`, then run:

```bash
node --env-file=.env.local --input-type=module <<'JS'
if (!process.env.INSFORGE_BASE_URL || !process.env.HEALTH_SYNC_TOKEN) {
  throw new Error('Set INSFORGE_BASE_URL and HEALTH_SYNC_TOKEN in .env.local');
}
const response = await fetch(`${process.env.INSFORGE_BASE_URL}/functions/google-health-sync`, {
  method: 'POST',
  headers: {
    'Content-Type': 'application/json',
    'x-health-sync-token': process.env.HEALTH_SYNC_TOKEN
  },
  body: JSON.stringify({ days_back: 7 })
});
console.log(response.status, await response.json());
if (!response.ok) process.exitCode = 1;
JS
```

Successful responses report the source, date range, row count, and sync time.
Use `days_back: 30` for the initial backfill if desired; the allowed range is
1–30 days. Compare today's dashboard steps, food calories, and protein with
the Google Health app. A successful HTTP response alone does not prove that
the account has the expected Fitbit or nutrition data.

To update automatically every 15 minutes, first check for an existing job:

```bash
npx -y @insforge/cli schedules list
```

If there is no Google Health job, create one:

```bash
npx -y @insforge/cli schedules create \
  --name "Google Health Sync" \
  --cron '*/15 * * * *' \
  --url 'https://your-project.insforge.app/functions/google-health-sync' \
  --method POST \
  --headers '{"Content-Type":"application/json","x-health-sync-token":"${{secrets.HEALTH_SYNC_TOKEN}}"}' \
  --body '{"days_back":7}'
```

Then inspect `schedules get <id>` and `schedules logs <id>` to verify the job
is active and completes successfully. If it already exists, update that job
instead of creating a duplicate. The dashboard refresh cadence is unchanged.

## Data semantics and failure behavior

- Steps use Google's reconciled **dailyRollUp** with the `google-wearables`
  source family, which includes Fitbit trackers and Pixel Watches, excluding
  iPhone steps and manual activity entries.
- Nutrition uses **nutrition-log** daily rollups with `all-sources`. Food
  calories come from `energy.kcalSum`; protein from the `PROTEIN` nutrient's
  `quantity.gramsSum`; carbs and fat from their dedicated totals. Burned
  calories are not used as food intake.
- Both reads must succeed before any rows are saved. Successful empty days
  become zeros, so deleting a food log clears the old totals. A failed API
  call or revoked permission leaves previously stored rows untouched.
- Google owns the entire displayed daily summary once connected. Steps and
  nutrition are never added to Apple totals or selected by latest writer.
- The function checks `x-health-sync-token` before contacting Google. OAuth
  credentials are used only on the server. It uses a refresh token to obtain
  short-lived access tokens, without storing or returning them to the Kindle.
- Device data reaches the API only after Fitbit syncs to Google Health.
  Scheduled polling does not make an offline Fitbit upload new data.

References: [steps](https://developers.google.com/health/data-types/steps),
[dailyRollUp](https://developers.google.com/health/reference/rest/v4/users.dataTypes.dataPoints/dailyRollUp),
[nutrition rollups](https://developers.google.com/health/reference/rest/v4/NutritionLogRollupValue).

## Local validation

```bash
npm run test:health
npm run check
npx -y deno check --no-config --no-lock --node-modules-dir=none functions/google-health-sync.ts functions/kindle-dashboard-data.ts functions/kindle-dashboard-events.ts
```

Tests use mocked Google and database responses. Real account permissions,
Fitbit totals, food-log coverage, deployment, and schedule execution require
the live checks above.
