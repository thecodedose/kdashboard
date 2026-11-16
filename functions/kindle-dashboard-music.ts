// Spotify credentials never leave this endpoint. Reuses the Kindle's device token.
type Playlist = { id: string; title: string; subtitle: string };
type PlaybackDevice = { id?: string; name?: string; is_restricted?: boolean };
class MusicError extends Error {
  constructor(public status: string, message: string, public retryAfter = 0) { super(message); }
}
let access = { token: "", expires: 0 };
let library = { items: [] as Playlist[], expires: 0 };

export default async function(req: Request): Promise<Response> {
  if (req.method === "OPTIONS") return json(null, 204);
  const deviceToken = Deno.env.get("DASHBOARD_TOGGLE_TOKEN");
  if (!deviceToken || req.headers.get("x-dashboard-toggle-token") !== deviceToken)
    return json({ ok: false, error: "Unauthorized" }, 401);
  if (!["GET", "POST"].includes(req.method)) return json({ ok: false, error: "Method not allowed" }, 405);
  let command: { action?: string; playlist_id?: string } = {};
  if (req.method === "POST") {
    try { command = await req.json(); } catch { return json({ ok: false, error: "Invalid JSON" }, 400); }
    if (!command || !["play", "pause", "next", "previous", "playlist"].includes(command.action || ""))
      return json({ ok: false, error: "Invalid music action" }, 400);
    if (command.action === "playlist" && !/^[A-Za-z0-9]{22}$/.test(command.playlist_id || ""))
      return json({ ok: false, error: "Invalid playlist" }, 400);
  }
  const base = { ok: true, provider: "spotify", playing: false, controllable: false, title: "", artist: "", device: "", playlists: [] as Playlist[] };
  if (!["SPOTIFY_CLIENT_ID", "SPOTIFY_CLIENT_SECRET", "SPOTIFY_REFRESH_TOKEN"].every(key => Deno.env.get(key)))
    return json({ ...base, status: "not_connected", notice: "Connect Spotify once to load your playlists." });
  try {
    const token = await accessToken();
    const playlists = await readPlaylists(token);
    const playback = await spotify("/me/player", token);
    const preferredName = Deno.env.get("SPOTIFY_DEVICE_NAME")?.trim();
    let device: PlaybackDevice | undefined = playback?.device;
    if (preferredName) {
      const available = await spotify("/me/player/devices", token);
      const matches = (Array.isArray(available?.devices) ? available.devices : [])
        .filter((candidate: PlaybackDevice) => candidate?.name === preferredName && candidate.id);
      // Never fall back to a phone/Mac, or guess between duplicate device names.
      device = matches.length === 1 ? matches[0] : undefined;
    }
    const observed = device?.id && device.id === playback?.device?.id ? playback : null;
    let notice = !device ? preferredName ? `Turn on ${clean(preferredName, 80)} and its speaker, then refresh.` : "Open Spotify on your phone or Mac and connect your Bluetooth speaker." :
      device.is_restricted ? "This playback device does not accept remote controls." : "Tap a playlist to play on this device.";
    let status = device && !device.is_restricted ? "connected" : "no_device";
    if (req.method === "POST") {
      if (!device || device.is_restricted || !device.id)
        return json({ ...base, playlists, status, notice });
      if (command.action === "playlist" && !playlists.some(p => p.id === command.playlist_id))
        return json({ ok: false, error: "Playlist is not in your dashboard library" }, 400);
      const route = command.action === "playlist" ? "play" : command.action;
      const alreadySet = observed && ((command.action === "play" && observed.is_playing === true) ||
        (command.action === "pause" && observed.is_playing === false));
      if (!alreadySet) {
        await spotify(`/me/player/${route}?device_id=${encodeURIComponent(device.id)}`, token, {
          method: route === "next" || route === "previous" ? "POST" : "PUT",
          ...(command.action === "playlist" ? { body: JSON.stringify({ context_uri: `spotify:playlist:${command.playlist_id}` }) } : {})
        });
      }
      // Acknowledgement is separate from observed playback; the next poll confirms it.
      notice = alreadySet ? "Playback is already up to date." : "Sent to Spotify. Updating playback...";
    }
    return json({ ...base, playlists, status, notice, device: clean(device?.name, 100),
      controllable: Boolean(device?.id && !device.is_restricted), playing: Boolean(observed?.is_playing),
      title: clean(observed?.item?.name, 160), artist: clean(observed?.item?.artists?.map((a: { name: string }) => a.name).join(", ") || observed?.item?.show?.name, 160) });
  } catch (error) {
    const problem = error instanceof MusicError ? error : new MusicError("unavailable", "Music unavailable. Refresh to try again.");
    return json({ ...base, playlists: library.items, status: problem.status, notice: problem.message, retry_after: problem.retryAfter });
  }
}

async function accessToken(): Promise<string> {
  if (access.token && Date.now() < access.expires) return access.token;
  const response = await fetch("https://accounts.spotify.com/api/token", {
    method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded", Authorization: `Basic ${btoa(`${Deno.env.get("SPOTIFY_CLIENT_ID")}:${Deno.env.get("SPOTIFY_CLIENT_SECRET")}`)}` },
    body: new URLSearchParams({ grant_type: "refresh_token", refresh_token: Deno.env.get("SPOTIFY_REFRESH_TOKEN")! }), signal: AbortSignal.timeout(10000)
  });
  if (!response.ok) throw new MusicError("not_connected", "Reconnect Spotify to restore music controls.");
  const data = await response.json();
  if (typeof data.access_token !== "string") throw new MusicError("not_connected", "Reconnect Spotify to restore music controls.");
  access = { token: data.access_token, expires: Date.now() + Math.max(0, (Number(data.expires_in) || 3600) - 60) * 1000 };
  return access.token;
}
async function spotify(path: string, token: string, options: RequestInit = {}) {
  const response = await fetch(`https://api.spotify.com/v1${path}`, {
    ...options, headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" }, signal: AbortSignal.timeout(10000)
  });
  if (response.status === 204) return null;
  if (response.status === 401) { access.expires = 0; throw new MusicError("not_connected", "Spotify session expired. Refresh or reconnect Spotify."); }
  if (response.status === 403) throw new MusicError("unavailable", "Spotify did not permit this request. Refresh playback or reconnect Spotify.");
  if (response.status === 404) throw new MusicError("no_device", "Playback device unavailable. Turn on the player, then refresh.");
  if (response.status === 429) throw new MusicError("rate_limited", "Spotify is busy. Please wait before refreshing.", Math.min(3600, Math.max(30, Number(response.headers.get("Retry-After")) || 30)));
  if (!response.ok) throw new MusicError("unavailable", "Spotify unavailable. Refresh to try again.");
  // Soloist commands can acknowledge with HTTP 200 and an opaque text ID.
  // Only read endpoints promise a JSON representation.
  if (options.method && options.method !== "GET") return null;
  return response.json();
}
async function readPlaylists(token: string): Promise<Playlist[]> {
  if (Date.now() < library.expires) return library.items;
  const data = await spotify("/me/playlists?limit=12", token);
  const items: Playlist[] = (Array.isArray(data?.items) ? data.items : []).filter((p: { id?: string }) => /^[A-Za-z0-9]{22}$/.test(p?.id || "")).slice(0, 12).map((p: { id: string; name: string; owner?: { display_name?: string } }) => ({
    id: p.id, title: clean(p.name, 120), subtitle: clean(p.owner?.display_name || "Spotify playlist", 120)
  }));
  library = { items, expires: Date.now() + 300000 };
  return items;
}
function clean(value: unknown, limit: number): string { return typeof value === "string" ? value.replace(/[\x00-\x1f]/g, " ").slice(0, limit) : ""; }
function json(body: unknown, status = 200): Response {
  return new Response(status === 204 ? null : JSON.stringify(body), { status, headers: {
    "Content-Type": "application/json", "Cache-Control": "no-store", "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS", "Access-Control-Allow-Headers": "Content-Type, X-Dashboard-Toggle-Token"
  } });
}
