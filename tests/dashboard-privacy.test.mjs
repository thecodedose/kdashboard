import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync, mkdtempSync, mkdirSync, writeFileSync, copyFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { execFileSync } from "node:child_process";
import vm from "node:vm";
import ts from "typescript";

function load(file, environment = {}, extras = {}) {
  const exports = {};
  const source = ts.transpileModule(readFileSync(file, "utf8"), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 }
  }).outputText;
  vm.runInNewContext(source + (file.endsWith("widgets.ts") ? "\nexports.dayStart = dayStart;" : ""), {
    exports, Request, Response, URL, URLSearchParams, TextEncoder, ReadableStream,
    Date, Intl, AbortSignal, performance, console: { log() {} },
    Deno: { env: { get: key => environment[key] } },
    require() { return { createAdminClient() { throw new Error("Unauthorized database read"); } }; },
    fetch() { throw new Error("Unexpected provider request"); },
    ...extras
  });
  return exports;
}

for (const name of ["data", "events", "version"]) {
  test(`${name} fails closed without a valid read credential, before database access`, async () => {
    const file = `functions/kindle-dashboard-${name}.ts`;
    for (const environment of [{ DASHBOARD_READ_TOKEN: "test-read" }, {}]) {
      const handler = load(file, environment).default;
      for (const headers of [{}, { "x-dashboard-read-token": "wrong" }, { "x-dashboard-toggle-token": "test-device" }]) {
        const response = await handler(new Request("https://example.invalid/dashboard", { headers }));
        assert.equal(response.status, 401);
        assert.doesNotMatch(await response.text(), /database|test-read|test-device/);
      }
      assert.equal((await handler(new Request("https://example.invalid/dashboard", { method: "OPTIONS" }))).status, 204);
    }
  });
}

test("authorized SSE sends only a version hash, with no health or list contents", async () => {
  const handler = load("functions/kindle-dashboard-events.ts", { DASHBOARD_READ_TOKEN: "test-read", INSFORGE_BASE_URL: "https://example.invalid", INSFORGE_API_KEY: "test-admin" }, {
    require() { return { createAdminClient() { return { database: { from(table) {
      const data = table === "planner_items" ? [{ text: "Private chore", updated_at: "2026-01-01" }] : [];
      const query = { select() { return this; }, in() { return this; }, eq() { return this; }, order() { return this; }, limit() { return this; }, then(resolve) { return Promise.resolve({ data, error: null }).then(resolve); } };
      return query;
    } } }; } }; },
    setInterval() { return 1; }, setTimeout() { return 2; }, clearInterval() {}, clearTimeout() {}
  }).default;
  const response = handler(new Request("https://example.invalid/events", { headers: { "x-dashboard-read-token": "test-read" } }));
  const reader = response.body.getReader();
  let text = "";
  while (!/event: planner(?:-error)?\ndata: [^\n]*\n\n/.test(text)) text += new TextDecoder().decode((await reader.read()).value);
  await reader.cancel();
  assert.match(text, /(?:^|\n)event: planner\ndata: [a-f0-9]+\n\n$/);
  assert.doesNotMatch(text, /Private chore|health|items|<header/);
});

test("Calendar day boundaries follow the configured timezone across DST", () => {
  const { dayStart } = load("functions/kindle-dashboard-widgets.ts");
  assert.equal(dayStart("2026-03-08", "America/New_York"), "2026-03-08T05:00:00.000Z");
  assert.equal(dayStart("2026-03-09", "America/New_York"), "2026-03-09T04:00:00.000Z");
  assert.equal(dayStart("2026-11-01", "America/New_York"), "2026-11-01T04:00:00.000Z");
  assert.equal(dayStart("2026-11-02", "America/New_York"), "2026-11-02T05:00:00.000Z");
  assert.equal(dayStart("2026-01-01", "UTC"), "2026-01-01T00:00:00.000Z");
});

test("native installer preserves existing device config and vacuum identity", () => {
  const root = mkdtempSync(path.join(tmpdir(), "dashboard-install-"));
  try {
    const write = (relative, value) => {
      const file = path.join(root, relative);
      mkdirSync(path.dirname(file), { recursive: true });
      writeFileSync(file, value);
    };
    write("volume/documents/.keep", "");
    write("volume/extensions/kindle-dashboard/config.sh", "# private device config\n");
    write("volume/extensions/kindle-dashboard/narwal-identity.txt", "test-identity\n");
    write("package/kindle-dashboard/bin/kindle-dashboard", "new binary\n");
    write("package/kindle-dashboard/config.sh.example", "# generic config\n");
    mkdirSync(path.join(root, "scripts"));
    mkdirSync(path.join(root, "kindle/native/build"), { recursive: true });
    copyFileSync("scripts/install-kindle-native.mjs", path.join(root, "scripts/install-kindle-native.mjs"));
    copyFileSync("kindle/launch-dashboard.sh", path.join(root, "kindle/launch-dashboard.sh"));
    execFileSync("tar", ["-C", path.join(root, "package"), "-czf", path.join(root, "kindle/native/build/kindle-dashboard-kual.tar.gz"), "kindle-dashboard"]);
    const environment = Object.fromEntries(Object.entries(process.env).filter(([key]) => !key.startsWith("DASHBOARD_")));
    execFileSync(process.execPath, ["scripts/install-kindle-native.mjs", path.join(root, "volume")], { cwd: root, env: environment, stdio: "pipe" });
    const extension = path.join(root, "volume/extensions/kindle-dashboard");
    assert.equal(readFileSync(path.join(extension, "config.sh"), "utf8"), "# private device config\n");
    assert.equal(readFileSync(path.join(extension, "narwal-identity.txt"), "utf8"), "test-identity\n");
    assert.equal(readFileSync(path.join(extension, "bin/kindle-dashboard"), "utf8"), "new binary\n");
  } finally { rmSync(root, { recursive: true, force: true }); }
});
