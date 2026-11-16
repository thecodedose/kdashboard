import { createAdminClient } from "npm:@insforge/sdk@1.5.2";

// Private data is requested only by a device with its existing toggle token.
// Neither notes nor calendar events are added to anonymous dashboard responses.
export default async function(req: Request): Promise<Response> {
  if (req.method === "OPTIONS") return json(null, 204);
  const token = Deno.env.get("DASHBOARD_TOGGLE_TOKEN");
  if (!token || req.headers.get("x-dashboard-toggle-token") !== token) return json({ ok: false, error: "Unauthorized" }, 401);
  if (!["GET", "PUT"].includes(req.method)) return json({ ok: false, error: "Method not allowed" }, 405);
  const admin = createAdminClient({ baseUrl: required("INSFORGE_BASE_URL"), apiKey: required("INSFORGE_API_KEY") });
  try {
    if (req.method === "PUT") {
      let body;
      try { body = await req.json(); } catch { return json({ ok: false, error: "Supply a JSON note" }, 400); }
      if (typeof body?.text !== "string" || new TextEncoder().encode(body.text).length > 16000 || body.text.includes("\0")) {
        return json({ ok: false, error: "Note must be text of at most 16000 UTF-8 bytes" }, 400);
      }
      const note = { id: "quick_note", text: body.text, updated_at: new Date().toISOString() };
      const { error } = await admin.database.from("dashboard_notes").upsert([note], { onConflict: "id" });
      if (error) throw new Error("Could not save quick note");
      return json({ ok: true, quick_note: note });
    }
    const date = new URL(req.url).searchParams.get("date") || localDate(new Date());
    if (!/^\d{4}-\d{2}-\d{2}$/.test(date) || !Number.isFinite(Date.parse(date)) || new Date(date).toISOString().slice(0, 10) !== date) {
      return json({ ok: false, error: "Invalid date" }, 400);
    }
    const [noteResult, calendar] = await Promise.all([
      admin.database.from("dashboard_notes").select("text,updated_at").eq("id", "quick_note").limit(1),
      readCalendar(date)
    ]);
    if (noteResult.error) throw new Error("Could not read quick note");
    return json({ ok: true, quick_note: noteResult.data?.[0] ?? { text: "", updated_at: null }, calendar });
  } catch {
    return json({ ok: false, error: "Widgets are temporarily unavailable" }, 503);
  }
}

type CalendarEvent = { summary?: string; location?: string; status?: string; start?: { date?: string; dateTime?: string }; end?: { date?: string; dateTime?: string } };
async function readCalendar(date: string) {
  const timezone = Deno.env.get("DASHBOARD_TIMEZONE") || "UTC";
  const base = { date, timezone, events: [] as { title: string; time: string; location: string; all_day: boolean }[] };
  if (!Deno.env.get("GOOGLE_CALENDAR_REFRESH_TOKEN")) return { ...base, status: "not_connected" };
  try {
    const tokenResponse = await fetch("https://oauth2.googleapis.com/token", {
      method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: new URLSearchParams({ grant_type: "refresh_token", client_id: required("GOOGLE_CALENDAR_CLIENT_ID"), client_secret: required("GOOGLE_CALENDAR_CLIENT_SECRET"), refresh_token: required("GOOGLE_CALENDAR_REFRESH_TOKEN") }),
      signal: AbortSignal.timeout(15000)
    });
    if (!tokenResponse.ok) throw new Error("Authorization unavailable");
    const token = await tokenResponse.json();
    if (typeof token.access_token !== "string" || !token.access_token) throw new Error("Missing access token");
    const endDate = new Date(`${date}T00:00:00Z`); endDate.setUTCDate(endDate.getUTCDate() + 1);
    const url = new URL(`https://www.googleapis.com/calendar/v3/calendars/${encodeURIComponent(Deno.env.get("GOOGLE_CALENDAR_ID") || "primary")}/events`);
    url.search = new URLSearchParams({ timeMin: dayStart(date, timezone), timeMax: dayStart(endDate.toISOString().slice(0, 10), timezone), timeZone: timezone, singleEvents: "true", orderBy: "startTime", maxResults: "100", fields: "items(summary,location,status,start,end),nextPageToken" }).toString();
    const events: CalendarEvent[] = [];
    for (let page = 0; page < 10; page++) {
      const response = await fetch(url, { headers: { Authorization: `Bearer ${token.access_token}` }, signal: AbortSignal.timeout(15000) });
      if (!response.ok) throw new Error("Calendar unavailable");
      const data = await response.json();
      if (!Array.isArray(data.items ?? [])) throw new Error("Invalid calendar response");
      events.push(...(data.items ?? []));
      if (!data.nextPageToken) break;
      if (page === 9) throw new Error("Too many calendar events");
      url.searchParams.set("pageToken", data.nextPageToken);
    }
    return { ...base, status: "connected", synced_at: new Date().toISOString(), events: events.filter(e => e.status !== "cancelled").slice(0, 100).map(event => ({
      title: (event.summary || "Untitled event").slice(0, 160), location: (event.location || "").slice(0, 100),
      all_day: Boolean(event.start?.date), time: eventTime(event, date, timezone)
    })), truncated: events.filter(e => e.status !== "cancelled").length > 100 };
  } catch { return { ...base, status: "unavailable" }; }
}

function eventTime(event: CalendarEvent, date: string, timezone: string): string {
  if (event.start?.date) return "All day";
  const start = new Date(event.start?.dateTime || "");
  const end = new Date(event.end?.dateTime || "");
  if (!Number.isFinite(start.getTime()) || !Number.isFinite(end.getTime())) return "Time unavailable";
  const clock = (value: Date) => new Intl.DateTimeFormat("en-US", { timeZone: timezone, hour: "numeric", minute: "2-digit", hour12: true }).format(value);
  const startText = localDate(start) < date ? "Earlier" : clock(start);
  const endText = localDate(end) > date ? "Next day" : clock(end);
  return `${startText} - ${endText}`;
}
function localDate(value: Date): string {
  const parts = new Intl.DateTimeFormat("en-CA", { timeZone: Deno.env.get("DASHBOARD_TIMEZONE") || "UTC", year: "numeric", month: "2-digit", day: "2-digit" }).formatToParts(value);
  const get = (key: string) => parts.find(part => part.type === key)?.value;
  return `${get("year")}-${get("month")}-${get("day")}`;
}
function dayStart(date: string, timezone: string): string {
  const target = Date.parse(`${date}T00:00:00Z`);
  let instant = target;
  const format = new Intl.DateTimeFormat("en-CA", { timeZone: timezone, year: "numeric", month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit", second: "2-digit", hourCycle: "h23" });
  for (let attempt = 0; attempt < 4; attempt++) {
    const parts = format.formatToParts(new Date(instant));
    const part = (key: string) => Number(parts.find(value => value.type === key)?.value);
    const displayed = Date.UTC(part("year"), part("month") - 1, part("day"), part("hour"), part("minute"), part("second"));
    if (displayed === target) return new Date(instant).toISOString();
    instant += target - displayed;
  }
  throw new Error("Could not resolve calendar day");
}
function required(key: string): string { const value = Deno.env.get(key); if (!value) throw new Error("Missing server configuration"); return value; }
function json(body: unknown, status = 200): Response {
  return new Response(status === 204 ? null : JSON.stringify(body), { status, headers: { "Content-Type": "application/json", "Cache-Control": "no-store", "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Methods": "GET, PUT, OPTIONS", "Access-Control-Allow-Headers": "Content-Type, X-Dashboard-Toggle-Token" } });
}
