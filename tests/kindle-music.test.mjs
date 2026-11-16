import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import ts from "typescript";

const playlistId = "1234567890123456789012";
function harness({ connected = true, device = true, restricted = false, failure = 0, tokenFailure = false, preferredName, availableDevices = [], playing = false, commandStatus = 204 } = {}) {
  const calls = [];
  const env = { DASHBOARD_TOGGLE_TOKEN: "device-token", ...(connected ? { SPOTIFY_CLIENT_ID: "client", SPOTIFY_CLIENT_SECRET: "client-secret", SPOTIFY_REFRESH_TOKEN: "refresh-secret" } : {}) };
  if (preferredName) env.SPOTIFY_DEVICE_NAME = preferredName;
  const exports = {};
  const source = ts.transpileModule(readFileSync("functions/kindle-dashboard-music.ts", "utf8"), { compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 } }).outputText;
  vm.runInNewContext(source, { exports, Deno: { env: { get: key => env[key] } }, Request, Response, URL, URLSearchParams, AbortSignal, Date, btoa,
    async fetch(url, options) {
      calls.push({ url: String(url), options });
      if (String(url).includes("accounts.spotify.com")) return Response.json(tokenFailure ? { error: "private error" } : { access_token: "access-secret", expires_in: 3600 }, { status: tokenFailure ? 400 : 200 });
      assert.equal(options.headers.Authorization, "Bearer access-secret");
      if (failure) return Response.json({ private: "upstream secret" }, { status: failure, headers: { "Retry-After": "60" } });
      if (String(url).includes("/me/playlists")) return Response.json({ items: [null, { id: "invalid", name: "Bad" }, { id: playlistId, name: "Morning", owner: { display_name: "Sample Owner" } }] });
      if (String(url).endsWith("/me/player/devices")) return Response.json({ devices: availableDevices });
      if (options.method) return new Response(commandStatus === 204 ? null : "opaque-command-id", { status: commandStatus });
      return Response.json({ device: device ? { id: "active-device", name: "Mac", is_restricted: restricted } : null, is_playing: playing, item: { name: "Track", artists: [{ name: "Artist" }] } });
    }
  });
  const request = (method = "GET", body, token = "device-token") => exports.default(new Request("https://example.invalid/music", { method, headers: { "x-dashboard-toggle-token": token }, ...(body === undefined ? {} : { body: JSON.stringify(body) }) }));
  return { request, calls };
}
test("music requires device authorization before any provider calls", async () => {
  const h = harness();
  assert.equal((await h.request("GET", undefined, "wrong")).status, 401);
  assert.equal(h.calls.length, 0);
  assert.equal((await h.request("DELETE")).status, 405);
});
test("the dedicated Pi can start a playlist while the Mac is the active device", async () => {
  const h = harness({ preferredName: "Kindle Music", availableDevices: [{ id: "pi-id", name: "Kindle Music", is_restricted: false }] });
  const snapshot = await (await h.request()).json();
  assert.equal(snapshot.device, "Kindle Music");
  assert.equal(snapshot.controllable, true);
  assert.equal(snapshot.title, "");
  assert.equal(snapshot.artist, "");
  assert.equal(snapshot.playing, false);
  await h.request("POST", { action: "playlist", playlist_id: playlistId });
  const command = h.calls.find(c => c.options.method === "PUT");
  assert.equal(command.url, "https://api.spotify.com/v1/me/player/play?device_id=pi-id");
  assert.deepEqual(JSON.parse(command.options.body), { context_uri: `spotify:playlist:${playlistId}` });
});
test("an unavailable, restricted, or ambiguous Pi never falls back to the Mac", async () => {
  for (const availableDevices of [[], [{ id: "mac", name: "Mac" }],
    [{ id: "pi", name: "Kindle Music", is_restricted: true }],
    [{ id: "pi-1", name: "Kindle Music" }, { id: "pi-2", name: "Kindle Music" }]]) {
    const h = harness({ preferredName: "Kindle Music", availableDevices });
    const result = await (await h.request("POST", { action: "play" })).json();
    assert.equal(result.status, "no_device");
    assert.equal(result.controllable, false);
    assert.equal(h.calls.filter(c => c.options.method && !c.url.includes("accounts.spotify.com")).length, 0);
  }
});
test("the Pi is controllable even when there is no active playback session", async () => {
  const h = harness({ device: false, preferredName: "Kindle Music", availableDevices: [{ id: "pi", name: "Kindle Music" }] });
  const result = await (await h.request("POST", { action: "play" })).json();
  assert.equal(result.controllable, true);
  assert.equal(h.calls.at(-1).url, "https://api.spotify.com/v1/me/player/play?device_id=pi");
});
test("unconnected music gives an honest setup state without provider calls", async () => {
  const h = harness({ connected: false });
  const result = await (await h.request()).json();
  assert.equal(result.status, "not_connected"); assert.equal(result.controllable, false);
  assert.deepEqual(result.playlists, []); assert.equal(h.calls.length, 0);
});
test("music snapshot returns bounded playlists and never provider credentials", async () => {
  const h = harness(); const result = await (await h.request()).json();
  assert.equal(result.status, "connected"); assert.equal(result.title, "Track");
  assert.equal(result.playlists.length, 1); assert.equal(result.playlists[0].title, "Morning");
  assert.doesNotMatch(JSON.stringify(result), /secret|access_token|refresh_token/);
  await h.request();
  assert.equal(h.calls.filter(c => c.url.includes("/me/playlists")).length, 1);
  assert.equal(h.calls.filter(c => c.url.includes("accounts.spotify.com")).length, 1);
});
test("playlist selection targets the active device and acknowledges without inventing playback", async () => {
  const h = harness();
  const result = await (await h.request("POST", { action: "playlist", playlist_id: playlistId })).json();
  const command = h.calls.find(c => c.options.method === "PUT");
  assert.equal(command.url, "https://api.spotify.com/v1/me/player/play?device_id=active-device");
  assert.deepEqual(JSON.parse(command.options.body), { context_uri: `spotify:playlist:${playlistId}` });
  assert.equal(result.playing, false); assert.match(result.notice, /Sent to Spotify/);
});
test("all transport controls use Spotify's documented methods", async () => {
  for (const action of ["play", "pause", "next", "previous"]) {
    const h = harness({ playing: action === "pause" }); await h.request("POST", { action });
    const command = h.calls.at(-1);
    assert.equal(command.url, `https://api.spotify.com/v1/me/player/${action}?device_id=active-device`);
    assert.equal(command.options.method, ["next", "previous"].includes(action) ? "POST" : "PUT");
  }
});
test("Soloist text acknowledgements are accepted for successful commands", async () => {
  const h = harness({ commandStatus: 200 });
  const result = await (await h.request("POST", { action: "play" })).json();
  assert.equal(result.status, "connected");
  assert.equal(result.controllable, true);
  assert.match(result.notice, /Sent to Spotify/);
});
test("repeating the current play or pause state does not trigger a restricted command", async () => {
  for (const [action, playing] of [["play", true], ["pause", false]]) {
    const h = harness({ playing });
    const result = await (await h.request("POST", { action })).json();
    assert.equal(result.status, "connected");
    assert.equal(result.playing, playing);
    assert.match(result.notice, /already up to date/);
    assert.equal(h.calls.filter(c => c.options.method && !c.url.includes("accounts.spotify.com")).length, 0);
  }
});
test("absent and restricted devices never receive playback commands", async () => {
  for (const config of [{ device: false }, { restricted: true }]) {
    const h = harness(config); const result = await (await h.request("POST", { action: "next" })).json();
    assert.equal(result.status, "no_device"); assert.equal(result.controllable, false);
    assert.equal(h.calls.filter(c => c.options.method && !c.url.includes("accounts.spotify.com")).length, 0);
  }
});
test("invalid actions and playlist IDs are rejected before provider calls", async () => {
  const h = harness();
  for (const body of [null, {}, { action: "transfer" }, { action: "playlist", playlist_id: "bad/../../id" }])
    assert.equal((await h.request("POST", body)).status, 400);
  assert.equal(h.calls.length, 0);
  assert.equal((await h.request("POST", { action: "playlist", playlist_id: "abcdefghijklmnopqrstuv" })).status, 400);
  assert.equal(h.calls.filter(c => c.options.method === "PUT").length, 0);
});
test("provider failures return useful states without leaking upstream responses", async () => {
  for (const failure of [401, 403, 404, 429, 500]) {
    const h = harness({ failure }); const result = await (await h.request()).json();
    assert.equal(result.controllable, false);
    assert.doesNotMatch(JSON.stringify(result), /upstream secret|access-secret|client-secret|refresh-secret/);
    if (failure === 429) { assert.equal(result.status, "rate_limited"); assert.equal(result.retry_after, 60); }
  }
  const h = harness({ tokenFailure: true });
  const result = await (await h.request()).json();
  assert.equal(result.status, "not_connected"); assert.doesNotMatch(JSON.stringify(result), /private error/);
});
