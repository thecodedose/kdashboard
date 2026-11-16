import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import ts from "typescript";
import * as crypto from "node:crypto";

const now = "2026-10-21T18:40:00Z"; // Oct 22 in the dashboard's India timezone.
class FixedDate extends Date {
  constructor(...args) { super(...(args.length ? args : [now])); }
  static now() { return new Date(now).getTime(); }
}
const env = {
  DASHBOARD_TIMEZONE: "Asia/Kolkata",
  GOOGLE_HEALTH_TIMEZONE: "Asia/Kolkata",
  HEALTH_SYNC_TOKEN: "test-sync-token",
  GOOGLE_HEALTH_CLIENT_ID: "test-client",
  GOOGLE_HEALTH_CLIENT_SECRET: "test-client-secret",
  GOOGLE_HEALTH_REFRESH_TOKEN: "test-refresh-token",
  INSFORGE_BASE_URL: "https://example.invalid",
  INSFORGE_API_KEY: "test-admin-key"
};
const normalize = (value) => JSON.parse(JSON.stringify(value));

function loadFunction(path, { createAdminClient, fetch, environment = env, expose = [] } = {}) {
  const source = ts.transpileModule(readFileSync(path, "utf8"), {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS }
  }).outputText;
  const module = { exports: {} };
  vm.runInNewContext(source + `\nexports.testing = {${expose.join(",")}};`, {
    exports: module.exports, module,
    require(name) {
      assert.match(name, /^npm:@insforge\/sdk(?:@1\.5\.2)?$/);
      return { createAdminClient };
    },
    Deno: { env: { get: (key) => environment[key] } },
    fetch, Request, Response, URL, URLSearchParams, AbortSignal,
    Date: FixedDate, Intl, performance,
    console: { log() {} }
  }, { filename: path });
  return module.exports;
}

function rollup(date, value) {
  const [year, month, day] = date.split("-").map(Number);
  return { civilStartTime: { date: { year, month, day } }, ...value };
}

function syncHarness({ steps = [], nutrition = [], environment = env, failure, tokenStatus = 200, scope, page } = {}) {
  const calls = [];
  const writes = [];
  const handler = loadFunction("functions/google-health-sync.ts", {
    environment,
    createAdminClient(config) {
      assert.equal(config.apiKey, env.INSFORGE_API_KEY);
      return { database: { from(table) {
        assert.equal(table, "health_daily_summaries");
        return { async upsert(rows, options) {
          writes.push({ rows: normalize(rows), options: normalize(options) });
          return { error: null };
        } };
      } } };
    },
    async fetch(url, options) {
      calls.push({ url, options });
      if (url === "https://oauth2.googleapis.com/token") {
        assert.equal(new URLSearchParams(options.body).get("grant_type"), "refresh_token");
        return Response.json(tokenStatus === 200 ? { access_token: "test-access-token", ...(scope ? { scope } : {}) } : { error: "private upstream response" }, { status: tokenStatus });
      }
      assert.equal(options.headers.Authorization, "Bearer test-access-token");
      const type = url.includes("/steps/") ? "steps" : "nutrition-log";
      if (failure === type) return Response.json({ error: "private upstream response" }, { status: 403 });
      if (page && type === "steps") return Response.json(page(JSON.parse(options.body)));
      return Response.json({ rollupDataPoints: type === "steps" ? steps : nutrition });
    }
  }).default;
  const request = (body = { days_back: 1 }, token = env.HEALTH_SYNC_TOKEN) => handler(new Request("https://example.invalid/google-health-sync", {
    method: "POST", headers: { "x-health-sync-token": token }, body: JSON.stringify(body)
  }));
  return { handler, request, calls, writes };
}

test("syncs Fitbit daily steps and Google food calories/macros into one provider row", async () => {
  const h = syncHarness({
    steps: [rollup("2026-10-22", { steps: { countSum: "8201" } })],
    nutrition: [rollup("2026-10-22", { nutritionLog: {
      energy: { kcalSum: 1650.255 }, energyFromFat: { kcalSum: 400 },
      nutrients: [{ nutrient: "PROTEIN", quantity: { gramsSum: 95.5 } }],
      totalCarbohydrate: { gramsSum: 180.25 }, totalFat: { gramsSum: 55 }
    } })]
  });
  assert.equal((await h.request()).status, 200);
  assert.deepEqual(h.writes[0].options, { onConflict: "date,source" });
  assert.deepEqual(h.writes[0].rows[0], {
    date: "2026-10-22", timezone: "Asia/Kolkata", source: "google_health",
    steps: 8201, dietary_energy_kcal: 1650.26, protein_g: 95.5, carbs_g: 180.25, fat_g: 55,
    synced_at: now.replace("Z", ".000Z")
  });
  const requests = h.calls.slice(1).map(({ options }) => JSON.parse(options.body));
  assert.deepEqual(requests[0].range, { start: { date: { year: 2026, month: 10, day: 22 } }, end: { date: { year: 2026, month: 10, day: 23 } } });
  assert.match(requests[0].dataSourceFamily, /google-wearables$/);
  assert.match(requests[1].dataSourceFamily, /all-sources$/);
  assert.equal(h.calls.some(({ url }) => /total-calories/.test(url)), false);
});

test("refreshes absent/deleted food totals to zero and does not reuse Apple nutrition", async () => {
  const h = syncHarness({ steps: [rollup("2026-10-22", { steps: { countSum: "0" } })] });
  assert.equal((await h.request()).status, 200);
  assert.equal(h.writes[0].rows[0].steps, 0);
  assert.equal(h.writes[0].rows[0].dietary_energy_kcal, 0);
  assert.equal(h.writes[0].rows[0].protein_g, 0);
  assert.equal(h.writes[0].rows[0].source, "google_health");
});

test("default sync covers seven complete calendar dates including today", async () => {
  const h = syncHarness();
  const response = await h.request({});
  assert.equal(response.status, 200);
  assert.deepEqual(h.writes[0].rows.map((row) => row.date), ["2026-10-16", "2026-10-17", "2026-10-18", "2026-10-19", "2026-10-20", "2026-10-21", "2026-10-22"]);
});

test("handles pagination without changing the aggregation request", async () => {
  const h = syncHarness({ page: (body) => body.pageToken
    ? { rollupDataPoints: [rollup("2026-10-22", { steps: { countSum: "42" } })] }
    : { rollupDataPoints: [], nextPageToken: "next" }
  });
  assert.equal((await h.request()).status, 200);
  assert.equal(h.writes[0].rows[0].steps, 42);
  const stepRequests = h.calls.filter(({ url }) => url.includes("/steps/")).map(({ options }) => JSON.parse(options.body));
  assert.deepEqual(stepRequests[1], { ...stepRequests[0], pageToken: "next" });
});

test("does not write any totals when either Google endpoint fails", async () => {
  for (const failure of ["steps", "nutrition-log"]) {
    const h = syncHarness({ failure });
    const response = await h.request();
    assert.equal(response.status, 502);
    assert.equal(h.writes.length, 0);
    assert.doesNotMatch(await response.text(), /private upstream response|test-access-token|test-client-secret/);
  }
});

test("requires both read scopes and handles revoked authorization without leaking tokens", async () => {
  for (const options of [{ tokenStatus: 400 }, { scope: "https://www.googleapis.com/auth/googlehealth.activity_and_fitness.readonly" }]) {
    const h = syncHarness(options);
    const response = await h.request();
    assert.equal(response.status, 502);
    assert.equal(h.writes.length, 0);
    assert.equal(h.calls.length, 1);
    assert.doesNotMatch(await response.text(), /private upstream response|test-refresh-token/);
  }
});

test("rejects unauthenticated requests and missing configuration before calling Google", async () => {
  const h = syncHarness();
  assert.equal((await h.request({}, "wrong-token")).status, 401);
  assert.equal(h.calls.length, 0);
  const missing = syncHarness({ environment: { HEALTH_SYNC_TOKEN: env.HEALTH_SYNC_TOKEN } });
  assert.equal((await missing.request()).status, 503);
  assert.equal(missing.calls.length, 0);
  assert.equal((await h.handler(new Request("https://example.invalid", { method: "GET" }))).status, 405);
});

test("bounds backfills and rejects invalid Google values before a database write", async () => {
  for (const days_back of [0, 31, 1.5, "7"]) {
    const h = syncHarness();
    assert.equal((await h.request({ days_back })).status, 400);
    assert.equal(h.calls.length, 0);
  }
  for (const steps of [
    [rollup("2026-10-22", { steps: { countSum: "-1" } })],
    [rollup("2026-10-22", { steps: { countSum: "NaN" } })],
    [rollup("2026-10-22", {}), rollup("2026-10-22", {})],
    [rollup("2026-10-23", { steps: { countSum: "1" } })]
  ]) {
    const h = syncHarness({ steps });
    assert.equal((await h.request()).status, 502);
    assert.equal(h.writes.length, 0);
  }
});

function dashboardHarness(file, rows, connected = true) {
  const queries = [];
  const tables = {
    health_daily_summaries: rows,
    health_targets: [{ metric: "steps", target_value: 10000, unit: "steps" }, { metric: "calories", target_value: 2000, unit: "kcal" }]
  };
  const loaded = loadFunction(`functions/${file}.ts`, {
    environment: { ...env, GOOGLE_HEALTH_REFRESH_TOKEN: connected ? env.GOOGLE_HEALTH_REFRESH_TOKEN : undefined },
    expose: [file === "kindle-dashboard-data" ? "loadDashboardPayload" : "loadDashboardData"],
    createAdminClient() {
      return { database: { from(table) {
        let result = [...(tables[table] ?? [])];
        const query = { table, filters: [] };
        queries.push(query);
        const builder = {
          select(columns) { query.columns = columns; return builder; },
          in(column, values) { query.filters.push([column, [...values]]); result = result.filter((row) => values.includes(row[column])); return builder; },
          eq(column, value) { query.filters.push([column, value]); result = result.filter((row) => row[column] === value); return builder; },
          order() { return builder; },
          limit(count) { query.limit = count; result = result.slice(0, count); return builder; },
          then(resolve) { return Promise.resolve({ data: result, error: null }).then(resolve); }
        };
        return builder;
      } } };
    }
  });
  return { queries, load: Object.values(loaded.testing)[0] };
}

for (const file of ["kindle-dashboard-data", "kindle-dashboard-events"]) {
  test(`${file}: prefers Google for both steps and nutrition, regardless of row order`, async () => {
    const apple = { source: "apple_health_ios", date: "2026-10-22", steps: 2000, dietary_energy_kcal: 900, protein_g: 20 };
    const google = { source: "google_health", date: "2026-10-22", steps: 8000, dietary_energy_kcal: 1700, protein_g: 90 };
    for (const rows of [[apple, google], [google, apple], [apple, { ...google, steps: 0, dietary_energy_kcal: 0, protein_g: 0 }]]) {
      const h = dashboardHarness(file, rows);
      const payload = await h.load();
      const expected = rows.find((row) => row.source === "google_health");
      assert.equal(payload.health.steps, expected.steps);
      assert.equal(payload.health[file === "kindle-dashboard-data" ? "calories" : "dietary_energy_kcal"], expected.dietary_energy_kcal);
      if (file === "kindle-dashboard-data") assert.equal(payload.health.protein_g, expected.protein_g);
      const query = h.queries.find(({ table }) => table === "health_daily_summaries");
      assert.equal(query.limit, 2);
      assert.deepEqual(query.filters, [["source", ["google_health", "apple_health_ios"]], ["date", "2026-10-22"]]);
    }
  });

  test(`${file}: retains Apple display until the first Google sync`, async () => {
    const h = dashboardHarness(file, [{ source: "apple_health_ios", date: "2026-10-22", steps: 321, dietary_energy_kcal: 456, protein_g: 20 }], false);
    assert.equal((await h.load()).health.steps, 321);
  });

  test(`${file}: never switches back to Apple if a connected Google sync is missing`, async () => {
    const h = dashboardHarness(file, [{ source: "apple_health_ios", date: "2026-10-22", steps: 321, dietary_energy_kcal: 456, protein_g: 20 }]);
    const payload = await h.load();
    if (file === "kindle-dashboard-data") {
      assert.equal(payload.health.steps, 0);
      assert.equal(payload.health.calories, 0);
      assert.equal(payload.health.synced_at, null);
    } else {
      assert.equal(payload.health, null);
    }
  });
}

test("OAuth helper validates callback state, uses PKCE, and saves credentials privately without logging them", async () => {
  const writes = [];
  const logs = [];
  const statuses = [];
  let requestHandler;
  let challenge;
  let closed = false;
  const fakeProcess = { argv: ["node", "script", "fixture.json"] };
  const scope = "https://www.googleapis.com/auth/googlehealth.activity_and_fitness.readonly https://www.googleapis.com/auth/googlehealth.nutrition.readonly";
  const source = ts.transpileModule(readFileSync("scripts/connect-google-health.mjs", "utf8"), {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS }
  }).outputText;
  const response = () => ({
    setHeader() {}, end() {},
    writeHead(status) { statuses.push(status); return this; }
  });
  const modules = {
    "node:crypto": crypto,
    "node:http": { createServer(handler) {
      requestHandler = handler;
      const server = {
        once() {},
        listen(port, host, ready) { assert.equal(host, "127.0.0.1"); ready(); },
        close() { closed = true; }, closeAllConnections() {}
      };
      return server;
    } },
    "node:child_process": { execFile(command, [url]) {
      assert.equal(command, "open");
      const auth = new URL(url);
      challenge = auth.searchParams.get("code_challenge");
      assert.equal(auth.searchParams.get("scope"), scope);
      assert.equal(auth.searchParams.get("code_challenge_method"), "S256");
      queueMicrotask(() => {
        requestHandler({ method: "GET", url: "/oauth/google-health/callback?state=wrong&code=injected" }, response());
        requestHandler({ method: "GET", url: `/oauth/google-health/callback?state=${auth.searchParams.get("state")}&code=valid-code` }, response());
      });
    } },
    "node:fs/promises": {
      async readFile() { return JSON.stringify({ web: { client_id: "client-id", client_secret: "private-client-secret", redirect_uris: ["http://127.0.0.1:8787/oauth/google-health/callback"] } }); },
      async mkdir(path, options) { assert.equal(options.mode, 0o700); },
      async chmod(path, mode) { assert.equal(mode, 0o700); },
      async writeFile(path, contents, options) { writes.push({ path, contents, options }); },
      async rename(from, to) { assert.equal(to, ".google-health/connection.json"); }
    }
  };
  await vm.runInNewContext(`(async () => {${source}\n})()`, {
    exports: {}, require: (name) => modules[name], process: fakeProcess,
    URL, URLSearchParams, AbortSignal, Date, setTimeout, clearTimeout,
    console: { log: (text) => logs.push(text), error: (text) => logs.push(text) },
    async fetch(url, options) {
      assert.equal(url, "https://oauth2.googleapis.com/token");
      const body = new URLSearchParams(options.body);
      assert.equal(body.get("code"), "valid-code");
      assert.equal(crypto.createHash("sha256").update(body.get("code_verifier")).digest("base64url"), challenge);
      return Response.json({ refresh_token: "private-refresh-token", scope });
    }
  });
  assert.deepEqual(statuses, [400]);
  assert.equal(closed, true);
  assert.equal(fakeProcess.exitCode, undefined);
  assert.equal(writes.length, 1);
  assert.equal(writes[0].options.mode, 0o600);
  assert.equal(writes[0].options.flag, "wx");
  assert.equal(JSON.parse(writes[0].contents).refresh_token, "private-refresh-token");
  assert.doesNotMatch(logs.join("\n"), /private-client-secret|private-refresh-token/);
});
