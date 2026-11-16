import { createServer } from "node:http";
import { randomBytes } from "node:crypto";
import { execFile } from "node:child_process";
import { chmod, mkdir, readFile, rename, writeFile } from "node:fs/promises";

const redirect = "http://127.0.0.1:8788/oauth/spotify/callback";
const scopes = ["user-read-playback-state", "user-modify-playback-state", "playlist-read-private", "playlist-read-collaborative"];
if (process.argv.includes("--help")) {
  console.log(`Usage: npm run music:connect -- [client.json]\nCreate a Spotify developer app with Web API access and redirect URI:\n  ${redirect}\nSave { "client_id": "...", "client_secret": "..." } privately in .spotify/client.json.\nSpotify Premium is required. The browser asks you to authorize playback and playlist access.\nNext: npm run music:configure`);
} else {
  try { await connect(); } catch (error) { console.error(error.message); process.exitCode = 1; }
}
async function connect() {
  let client;
  try { client = JSON.parse(await readFile(process.argv[2] || ".spotify/client.json", "utf8")); }
  catch { throw new Error("Save your Spotify client in .spotify/client.json. See npm run music:connect -- --help."); }
  if (typeof client.client_id !== "string" || typeof client.client_secret !== "string" || !client.client_id || !client.client_secret)
    throw new Error("The client JSON needs client_id and client_secret.");
  const state = randomBytes(32).toString("base64url");
  const url = new URL("https://accounts.spotify.com/authorize");
  url.search = new URLSearchParams({ client_id: client.client_id, response_type: "code", redirect_uri: redirect, state, scope: scopes.join(" ") }).toString();
  let server, timer;
  const pending = new Promise((resolve, reject) => {
    server = createServer((req, res) => {
      const callback = new URL(req.url, redirect);
      res.setHeader("Cache-Control", "no-store");
      res.setHeader("Content-Type", "text/plain; charset=utf-8");
      if (req.method !== "GET" || callback.pathname !== "/oauth/spotify/callback") return res.writeHead(404).end("Not found");
      if (callback.searchParams.get("state") !== state) return res.writeHead(400).end("Invalid sign-in state. Return to the original sign-in tab.");
      if (callback.searchParams.has("error") || !callback.searchParams.get("code")) {
        res.writeHead(400).end("Authorization was not granted.");
        reject(new Error("Spotify authorization was not granted. Run music:connect to retry."));
        return;
      }
      res.end("Spotify authorized. You can close this tab and return to Codex.");
      resolve(callback.searchParams.get("code"));
    });
    server.once("error", () => reject(new Error("Could not start the Spotify callback on port 8788.")));
    server.listen(8788, "127.0.0.1", () => {
      console.log(`Authorize Spotify in your browser:\n${url}`);
      if (!process.env.SPOTIFY_OAUTH_NO_OPEN) execFile("open", [url.toString()], () => {});
    });
    timer = setTimeout(() => reject(new Error("Spotify sign-in timed out. Run music:connect to retry.")), 300000);
  });
  let code;
  try { code = await pending; }
  finally { clearTimeout(timer); server.close(); server.closeAllConnections(); }
  const response = await fetch("https://accounts.spotify.com/api/token", {
    method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded", Authorization: `Basic ${Buffer.from(`${client.client_id}:${client.client_secret}`).toString("base64")}` },
    body: new URLSearchParams({ grant_type: "authorization_code", code, redirect_uri: redirect }), signal: AbortSignal.timeout(15000)
  });
  if (!response.ok) throw new Error(`Spotify authorization failed (HTTP ${response.status}). Check your client and redirect URI.`);
  const token = await response.json();
  if (!token.refresh_token || scopes.some(scope => !token.scope?.split(" ").includes(scope)))
    throw new Error("Spotify did not grant the required playlist and playback permissions. Reconnect and allow them.");
  await mkdir(".spotify", { recursive: true, mode: 0o700 });
  await chmod(".spotify", 0o700);
  const temporary = `.spotify/connection-${randomBytes(8).toString("hex")}.tmp`;
  await writeFile(temporary, JSON.stringify({ client_id: client.client_id, client_secret: client.client_secret, refresh_token: token.refresh_token, scope: token.scope, connected_at: new Date().toISOString() }, null, 2) + "\n", { mode: 0o600, flag: "wx" });
  await rename(temporary, ".spotify/connection.json");
  console.log("Spotify connected privately. Next: npm run music:configure");
}
