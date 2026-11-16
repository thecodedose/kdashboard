import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import ts from "typescript";

function harness({ connected = true, events = [], failToken = false, pages } = {}) {
  const calls = [], writes = [];
  const environment = { DASHBOARD_TIMEZONE: "Asia/Kolkata", DASHBOARD_READ_TOKEN: "read-token", DASHBOARD_TOGGLE_TOKEN: "device-token", INSFORGE_BASE_URL: "https://example.invalid", INSFORGE_API_KEY: "admin-key", ...(connected ? { GOOGLE_CALENDAR_REFRESH_TOKEN: "refresh-secret", GOOGLE_CALENDAR_CLIENT_ID: "client", GOOGLE_CALENDAR_CLIENT_SECRET: "client-secret" } : {}) };
  const source = ts.transpileModule(readFileSync("functions/kindle-dashboard-widgets.ts", "utf8"), { compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 } }).outputText;
  const exports = {};
  vm.runInNewContext(source, { exports, require: () => ({ createAdminClient() { return { database: { from(table) {
    assert.equal(table, "dashboard_notes");
    return { select() { return this; }, eq() { return this; }, async limit() { return { data: [{ text: "Private note\nSecond paragraph", updated_at: "2026-10-22T12:00:00Z" }], error: null }; }, async upsert(rows, options) { writes.push({ rows, options }); return { error: null }; } };
  } } }; } }), Deno: { env: { get: key => environment[key] } }, Request, Response, URL, URLSearchParams, AbortSignal, Date, Intl, TextEncoder,
    async fetch(url, options) {
      calls.push({ url: String(url), options });
      if (String(url).includes("oauth2.googleapis.com")) return Response.json(failToken ? { error: "upstream-private-error" } : { access_token: "access-secret" }, { status: failToken ? 400 : 200 });
      const parsed = new URL(url);
      assert.equal(parsed.searchParams.get("singleEvents"), "true");
      assert.equal(parsed.searchParams.get("timeMin"), "2026-10-21T18:30:00.000Z");
      assert.equal(parsed.searchParams.get("timeMax"), "2026-10-22T18:30:00.000Z");
      assert.equal(options.headers.Authorization, "Bearer access-secret");
      return Response.json(pages ? pages(parsed.searchParams.get("pageToken")) : { items: events });
    }
  });
  const request = (method = "GET", body, token = "device-token", date = "2026-10-22") => exports.default(new Request(`https://example.invalid/widgets?date=${date}`, { method, headers: { "x-dashboard-toggle-token": token }, ...(body === undefined ? {} : { body: JSON.stringify(body) }) }));
  return { request, calls, writes };
}

test("private widgets reject an absent/wrong device token before reads or Google calls", async () => {
  const h = harness(); const response = await h.request("GET", undefined, "wrong");
  assert.equal(response.status, 401); assert.equal(h.calls.length, 0);
  assert.doesNotMatch(await response.text(), /Private note|refresh-secret|client-secret/);
});
test("quick notes preserve paragraphs and reject oversized UTF-8 input", async () => {
  const h = harness(); const text = "Remember the bag.\n\nCall the electrician.";
  assert.equal((await h.request("PUT", { text })).status, 200);
  assert.equal(h.writes[0].rows[0].text, text); assert.equal(h.writes[0].options.onConflict, "id");
  assert.equal((await h.request("PUT", { text: "a".repeat(16001) })).status, 400);
  assert.equal((await h.request("PUT", { text: "é".repeat(8001) })).status, 400);
  assert.equal((await h.request("PUT", { text: "\0" })).status, 400);
  assert.equal(h.writes.length, 1);
});
test("day agenda includes all-day and overnight events and omits cancellations", async () => {
  const h = harness({ events: [
    { summary: "All-day reminder", start: { date: "2026-10-22" }, end: { date: "2026-10-23" } },
    { summary: "Late event", start: { dateTime: "2026-10-21T23:30:00+05:30" }, end: { dateTime: "2026-10-22T01:00:00+05:30" } },
    { summary: "Cancelled", status: "cancelled" }
  ] });
  const response = await h.request(); const data = await response.json();
  assert.equal(data.calendar.status, "connected"); assert.equal(data.calendar.events.length, 2);
  assert.equal(data.calendar.events[0].time, "All day"); assert.equal(data.calendar.events[1].time, "Earlier - 1:00 AM");
  assert.equal(data.quick_note.text, "Private note\nSecond paragraph");
  assert.doesNotMatch(JSON.stringify(data), /access-secret|refresh-secret|client-secret/);
});
test("calendar pagination retains events across pages", async () => {
  const h = harness({ pages: token => token ? { items: [{ summary: "Second", start: { date: "2026-10-22" } }] } : { items: [{ summary: "First", start: { date: "2026-10-22" } }], nextPageToken: "page2" } });
  const data = await (await h.request()).json(); assert.equal(data.calendar.events.length, 2);
});
test("revoked Calendar access and disconnected setup keep the note usable", async () => {
  for (const options of [{ failToken: true }, { connected: false }]) {
    const h = harness(options); const response = await h.request(); const data = await response.json();
    assert.equal(response.status, 200); assert.match(data.quick_note.text, /Private note/);
    assert.equal(data.calendar.status, options.connected === false ? "not_connected" : "unavailable");
    assert.doesNotMatch(JSON.stringify(data), /upstream-private-error|refresh-secret/);
  }
});
test("invalid calendar dates fail before calling Google", async () => {
  const h = harness(); assert.equal((await h.request("GET", undefined, "device-token", "2026-02-31")).status, 400);
  assert.equal(h.calls.length, 0);
});

test("dashboard reads require a read token and private widgets require the device token", async () => {
  const exports = {}; let privateReads = 0;
  const environment = { DASHBOARD_TIMEZONE: "Asia/Kolkata", DASHBOARD_READ_TOKEN: "read-token", DASHBOARD_TOGGLE_TOKEN: "device-token", INSFORGE_BASE_URL: "https://example.invalid", INSFORGE_API_KEY: "admin-key" };
  const source = ts.transpileModule(readFileSync("functions/kindle-dashboard-data.ts", "utf8"), { compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 } }).outputText;
  vm.runInNewContext(source, { exports, require: () => ({ createAdminClient() { return { database: { from() {
    const query = { select() { return this; }, in() { return this; }, eq() { return this; }, order() { return this; }, limit() { return this; }, then(resolve) { return Promise.resolve({ data: [], error: null }).then(resolve); } }; return query;
  } } }; } }), Deno: { env: { get: key => environment[key] } }, Request, Response, URL, URLSearchParams, AbortSignal, Date, Intl, performance, console: { log() {} },
    async fetch(url, options) { privateReads++; assert.equal(options.headers["x-dashboard-toggle-token"], "device-token"); return Response.json({ quick_note: { text: "Private message" }, calendar: { status: "connected", events: [] } }); }
  });
  for (const headers of [{}, { "x-dashboard-read-token": "wrong" }, { "x-dashboard-toggle-token": "device-token" }]) {
    const response = await exports.default(new Request("https://example.invalid/data", { headers }));
    assert.equal(response.status, 401);
    assert.doesNotMatch(await response.text(), /Private message/);
  }
  assert.equal(privateReads, 0);
  for (const token of [undefined, "wrong"]) {
    const result = await (await exports.default(new Request("https://example.invalid/data", { headers: { "x-dashboard-read-token": "read-token", ...(token ? { "x-dashboard-toggle-token": token } : {}) } }))).json();
    assert.equal(result.ok, true); assert.equal("quick_note" in result, false); assert.equal("calendar" in result, false);
  }
  assert.equal(privateReads, 0);
  const result = await (await exports.default(new Request("https://example.invalid/data", { headers: { "x-dashboard-read-token": "read-token", "x-dashboard-toggle-token": "device-token" } }))).json();
  assert.equal(result.quick_note.text, "Private message"); assert.equal(privateReads, 1);
});
