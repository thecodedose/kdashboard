import { createAdminClient } from "npm:@insforge/sdk@1.5.2";

const SOURCE = "google_health";
const SCOPES = [
  "https://www.googleapis.com/auth/googlehealth.activity_and_fitness.readonly",
  "https://www.googleapis.com/auth/googlehealth.nutrition.readonly"
];

type CivilDate = { year: number; month: number; day: number };
type Rollup = {
  civilStartTime: { date: CivilDate };
  steps?: { countSum?: string };
  nutritionLog?: {
    energy?: { kcalSum?: number };
    nutrients?: { nutrient: string; quantity?: { gramsSum?: number } }[];
    totalCarbohydrate?: { gramsSum?: number };
    totalFat?: { gramsSum?: number };
  };
};

// Single-owner dashboard: OAuth credentials stay in server secrets. The caller
// supplies only the existing sync token, never a Google access token or user ID.
export default async function(req: Request): Promise<Response> {
  if (req.method === "OPTIONS") return jsonResponse(null, 204);
  if (req.method !== "POST") return jsonResponse({ ok: false, error: "Method not allowed" }, 405);

  const syncToken = Deno.env.get("HEALTH_SYNC_TOKEN");
  if (!syncToken) return jsonResponse({ ok: false, error: "Health sync is not configured" }, 503);
  if (req.headers.get("x-health-sync-token") !== syncToken) {
    return jsonResponse({ ok: false, error: "Unauthorized" }, 401);
  }

  const credentialKeys = ["GOOGLE_HEALTH_CLIENT_ID", "GOOGLE_HEALTH_CLIENT_SECRET", "GOOGLE_HEALTH_REFRESH_TOKEN"];
  if (credentialKeys.some((key) => !Deno.env.get(key))) {
    return jsonResponse({ ok: false, error: "Google Health is not connected. Complete health:connect and configure the backend secrets." }, 503);
  }

  let daysBack = 7;
  try {
    const text = await req.text();
    const body = text.trim() ? JSON.parse(text) : {};
    if (!body || typeof body !== "object" || Array.isArray(body)) throw new Error();
    daysBack = body.days_back ?? 7;
    if (!Number.isInteger(daysBack) || daysBack < 1 || daysBack > 30) throw new Error();
  } catch {
    return jsonResponse({ ok: false, error: "Supply days_back as an integer from 1 to 30" }, 400);
  }

  try {
    const timezone = Deno.env.get("GOOGLE_HEALTH_TIMEZONE") || "UTC";
    const today = localDate(new Date(), timezone);
    const start = addDays(today, -(daysBack - 1));
    const end = addDays(today, 1);
    const accessToken = await refreshAccessToken();
    // Fetch both before writing. A nutrition permission/API failure must not
    // replace previously synced food totals with zeroes.
    const [steps, nutrition] = await Promise.all([
      dailyRollups("steps", start, end, accessToken, "google-wearables"),
      dailyRollups("nutrition-log", start, end, accessToken, "all-sources")
    ]);
    const stepDays = rollupsByDate(steps, start, end);
    const nutritionDays = rollupsByDate(nutrition, start, end);
    const syncedAt = new Date().toISOString();
    const rows = Array.from({ length: daysBack }, (_, offset) => {
      const date = addDays(start, offset);
      const food = nutritionDays.get(date)?.nutritionLog;
      const protein = food?.nutrients?.find((item) => item.nutrient === "PROTEIN");
      return {
        date,
        timezone,
        source: SOURCE,
        steps: stepCount(stepDays.get(date)?.steps?.countSum),
        dietary_energy_kcal: nutritionValue(food?.energy?.kcalSum),
        protein_g: nutritionValue(protein?.quantity?.gramsSum),
        carbs_g: nutritionValue(food?.totalCarbohydrate?.gramsSum),
        fat_g: nutritionValue(food?.totalFat?.gramsSum),
        synced_at: syncedAt
      };
    });

    const admin = createAdminClient({
      baseUrl: requiredEnv("INSFORGE_BASE_URL"),
      apiKey: requiredEnv("INSFORGE_API_KEY")
    });
    // Atomic conflict handling also makes retries/overlapping scheduled calls
    // safe. Each provider owns a separate row under (date, source).
    const { error } = await admin.database.from("health_daily_summaries")
      .upsert(rows, { onConflict: "date,source" });
    if (error) throw new Error("Could not save Google Health totals. Retry the sync.");

    return jsonResponse({ ok: true, source: SOURCE, rows: rows.length, from: start, to: today, synced_at: syncedAt });
  } catch (error) {
    return jsonResponse({ ok: false, error: error instanceof Error ? error.message : "Google Health sync failed" }, 502);
  }
}

async function refreshAccessToken(): Promise<string> {
  const response = await fetch("https://oauth2.googleapis.com/token", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams({
      grant_type: "refresh_token",
      client_id: requiredEnv("GOOGLE_HEALTH_CLIENT_ID"),
      client_secret: requiredEnv("GOOGLE_HEALTH_CLIENT_SECRET"),
      refresh_token: requiredEnv("GOOGLE_HEALTH_REFRESH_TOKEN")
    }),
    signal: AbortSignal.timeout(15000)
  });
  if (!response.ok) {
    throw new Error(`Google authorization failed (HTTP ${response.status}). Reconnect with npm run health:connect.`);
  }
  const token = await response.json();
  if (typeof token.access_token !== "string" || !token.access_token) throw new Error("Google returned no access token");
  if (token.scope && SCOPES.some((scope) => !token.scope.split(" ").includes(scope))) {
    throw new Error("Google Health needs both activity and nutrition read permissions. Reconnect with health:connect.");
  }
  return token.access_token;
}

async function dailyRollups(dataType: string, start: string, end: string, accessToken: string, family: string): Promise<Rollup[]> {
  const results: Rollup[] = [];
  let pageToken: string | undefined;
  for (let page = 0; page < 10; page++) {
    const response = await fetch(`https://health.googleapis.com/v4/users/me/dataTypes/${dataType}/dataPoints:dailyRollUp`, {
      method: "POST",
      headers: { Authorization: `Bearer ${accessToken}`, "Content-Type": "application/json" },
      body: JSON.stringify({
        range: { start: { date: civilDate(start) }, end: { date: civilDate(end) } },
        windowSizeDays: 1,
        pageSize: 90,
        dataSourceFamily: `users/me/dataSourceFamilies/${family}`,
        ...(pageToken ? { pageToken } : {})
      }),
      signal: AbortSignal.timeout(15000)
    });
    if (!response.ok) throw new Error(`Google Health ${dataType} request failed (HTTP ${response.status}). Check API access and permissions.`);
    const payload = await response.json();
    if (payload.rollupDataPoints !== undefined && !Array.isArray(payload.rollupDataPoints)) {
      throw new Error(`Invalid Google Health ${dataType} response`);
    }
    results.push(...(payload.rollupDataPoints ?? []));
    pageToken = payload.nextPageToken;
    if (!pageToken) return results;
  }
  throw new Error("Google Health returned too many pages; no totals were saved");
}

function rollupsByDate(rollups: Rollup[], start: string, end: string): Map<string, Rollup> {
  const days = new Map<string, Rollup>();
  for (const rollup of rollups) {
    const date = rollup.civilStartTime?.date;
    if (!date || ![date.year, date.month, date.day].every(Number.isInteger)) throw new Error("Invalid Google Health rollup date");
    const key = `${String(date.year).padStart(4, "0")}-${String(date.month).padStart(2, "0")}-${String(date.day).padStart(2, "0")}`;
    if (addDays(key, 0) !== key || key < start || key >= end || days.has(key)) throw new Error("Unexpected Google Health rollup date");
    days.set(key, rollup);
  }
  return days;
}

function stepCount(value: string | undefined): number {
  const count = Number(value ?? 0);
  if (!Number.isInteger(count) || count < 0 || count > 2147483647) throw new Error("Invalid Google Health step total");
  return count;
}

function nutritionValue(value: number | undefined): number {
  const number = value ?? 0;
  if (typeof number !== "number" || !Number.isFinite(number) || number < 0 || number > 99999999.99) throw new Error("Invalid Google Health nutrition total");
  return Math.round(number * 100) / 100;
}

function localDate(now: Date, timezone: string): string {
  const parts = new Intl.DateTimeFormat("en-US", { timeZone: timezone, year: "numeric", month: "2-digit", day: "2-digit" }).formatToParts(now);
  const part = (type: string) => parts.find((entry) => entry.type === type)?.value;
  return `${part("year")}-${part("month")}-${part("day")}`;
}

function civilDate(date: string): CivilDate {
  const [year, month, day] = date.split("-").map(Number);
  return { year, month, day };
}

function addDays(date: string, count: number): string {
  const day = new Date(`${date}T12:00:00Z`);
  day.setUTCDate(day.getUTCDate() + count);
  return day.toISOString().slice(0, 10);
}

function requiredEnv(key: string): string {
  const value = Deno.env.get(key);
  if (!value) throw new Error(`Missing ${key}`);
  return value;
}

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(status === 204 ? null : JSON.stringify(body), {
    status,
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      "Cache-Control": "no-store",
      "Access-Control-Allow-Origin": "*",
      "Access-Control-Allow-Methods": "POST, OPTIONS",
      "Access-Control-Allow-Headers": "Content-Type, X-Health-Sync-Token"
    }
  });
}
