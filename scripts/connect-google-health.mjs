import { createServer } from "node:http";
import { createHash, randomBytes } from "node:crypto";
import { execFile } from "node:child_process";
import { chmod, mkdir, readFile, rename, writeFile } from "node:fs/promises";

const redirectUri = "http://127.0.0.1:8787/oauth/google-health/callback";
const scopes = [
  "https://www.googleapis.com/auth/googlehealth.activity_and_fitness.readonly",
  "https://www.googleapis.com/auth/googlehealth.nutrition.readonly"
];

if (process.argv.includes("--help")) {
  console.log(`Usage: npm run health:connect -- [path/to/client-secret.json]

Create a Google Cloud Web application OAuth client with redirect URI:
  ${redirectUri}
Enable Google Health API and add your Google account as a test user.
Default client file: .google-health/client-secret.json
The browser asks for activity and nutrition READ access only.
Credentials are saved privately to .google-health/connection.json.
See docs/google-health.md for the full setup.`);
} else {
  try {
    await connect();
  } catch (error) {
    console.error(error.message);
    process.exitCode = 1;
  }
}

async function connect() {
  const clientFile = process.argv[2] || ".google-health/client-secret.json";
  let client;
  try {
    client = JSON.parse(await readFile(clientFile, "utf8")).web;
  } catch {
    throw new Error("Could not read the OAuth client JSON. See npm run health:connect -- --help.");
  }
  if (!client?.client_id || !client?.client_secret || !client.redirect_uris?.includes(redirectUri)) {
    throw new Error(`Use a Web application OAuth client with ${redirectUri} in its authorized redirect URIs.`);
  }
  const state = randomBytes(32).toString("base64url");
  const verifier = randomBytes(32).toString("base64url");
  const authUrl = new URL("https://accounts.google.com/o/oauth2/v2/auth");
  authUrl.search = new URLSearchParams({
    client_id: client.client_id,
    redirect_uri: redirectUri,
    response_type: "code",
    access_type: "offline",
    prompt: "consent",
    scope: scopes.join(" "),
    state,
    code_challenge: createHash("sha256").update(verifier).digest("base64url"),
    code_challenge_method: "S256"
  }).toString();

  let server;
  let timer;
  const codePromise = new Promise((resolve, reject) => {
    server = createServer((req, res) => {
      const url = new URL(req.url, redirectUri);
      res.setHeader("Cache-Control", "no-store");
      res.setHeader("Content-Type", "text/plain; charset=utf-8");
      if (url.pathname !== "/oauth/google-health/callback" || req.method !== "GET") {
        res.writeHead(404).end("Not found");
        return;
      }
      if (url.searchParams.get("state") !== state) {
        res.writeHead(400).end("Invalid authorization state. Return to the original sign-in tab.");
        return;
      }
      const code = url.searchParams.get("code");
      if (url.searchParams.has("error") || !code) {
        res.writeHead(400).end("Authorization was not granted. You can close this tab.");
        reject(new Error("Google authorization was not granted. Run health:connect to retry."));
        return;
      }
      res.end("Authorization received. Return to the terminal to finish connecting Google Health.");
      resolve(code);
    });
    server.once("error", () => reject(new Error("Could not start the local OAuth callback on port 8787. Close the other process and retry.")));
    server.listen(8787, "127.0.0.1", () => {
      console.log("Opening Google sign-in. Choose the account used by your Fitbit and allow activity and nutrition read access.");
      console.log(`If the browser does not open, visit:\n${authUrl}`);
      if (!process.env?.GOOGLE_OAUTH_NO_OPEN) execFile("open", [authUrl.toString()], () => {});
    });
    timer = setTimeout(() => reject(new Error("Google sign-in timed out. Run health:connect to retry.")), 5 * 60 * 1000);
  });

  let code;
  try {
    code = await codePromise;
  } finally {
    clearTimeout(timer);
    server.close();
    server.closeAllConnections();
  }

  const response = await fetch("https://oauth2.googleapis.com/token", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams({
      client_id: client.client_id,
      client_secret: client.client_secret,
      grant_type: "authorization_code",
      redirect_uri: redirectUri,
      code,
      code_verifier: verifier
    }),
    signal: AbortSignal.timeout(15000)
  });
  if (!response.ok) throw new Error(`Google token exchange failed (HTTP ${response.status}). Run health:connect to retry.`);
  const token = await response.json();
  if (!token.refresh_token || scopes.some((scope) => !token.scope?.split(" ").includes(scope))) {
    throw new Error("Both activity and nutrition read permissions, with offline access, are required. Run health:connect and grant both permissions.");
  }
  await mkdir(".google-health", { recursive: true, mode: 0o700 });
  await chmod(".google-health", 0o700);
  const tempFile = `.google-health/connection-${randomBytes(8).toString("hex")}.tmp`;
  await writeFile(tempFile, JSON.stringify({
    client_id: client.client_id,
    client_secret: client.client_secret,
    refresh_token: token.refresh_token,
    scope: token.scope,
    connected_at: new Date().toISOString(),
    ...(token.refresh_token_expires_in ? { refresh_token_expires_at: new Date(Date.now() + token.refresh_token_expires_in * 1000).toISOString() } : {})
  }, null, 2) + "\n", { mode: 0o600, flag: "wx" });
  await rename(tempFile, ".google-health/connection.json");
  console.log("Google authorization saved privately. Next: npm run health:configure");
  if (token.refresh_token_expires_in) {
    console.log("Google issued a time-limited refresh token. Reconnect when it expires; Testing-mode apps commonly require weekly reconnection.");
  }
}
