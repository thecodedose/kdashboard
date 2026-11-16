# Spotify music

The Music page shows playlists and controls playback through the token-protected
`kindle-dashboard-music` function. Connect your own Spotify account; no playlists,
account credentials, speaker pairing or player session are included in this kit.

## Connect the widget

1. Create a Spotify Web API application and register the callback
   `http://127.0.0.1:8788/oauth/spotify/callback`.
2. Save `{ "client_id": "...", "client_secret": "..." }` in the ignored
   `.spotify/client.json` file with owner-only permissions.
3. Run `npm run music:connect`, then `npm run music:configure` against your backend.
4. Deploy with `npm run kit:backend`. Set the server secret `SPOTIFY_DEVICE_NAME`
   to the exact player name if you want a dedicated playback device. Otherwise
   the endpoint controls the current active device.
5. Open Music on the Kindle and refresh. The page loads up to 12 playlists.

The OAuth helper requests playlist read and playback read/control scopes. Web API
credentials stay server-side; the Kindle uses its existing dashboard toggle token.
If a named device is missing, restricted or ambiguous, controls remain unavailable.
The function never silently transfers playback to another device. A sent command
is acknowledged separately from playback observed during the next poll.

## Optional direct Kindle playback

The optional KUAL player uses Soloist with an isolated ARM runtime and the Kindle's
native Bluetooth audio output. It requires a compatible jailbroken Kindle, Wi-Fi,
an already paired speaker, your own Soloist key and a suitable Spotify account.
Hardware support must be checked on your device.

1. Put your Soloist key in `.spotify/soloist.key` (directory mode `0700`, file mode
   `0600`). Obtain the player/key through your own Spotify developer account.
2. Build with `npm run music:build -- --zig /path/to/zig`. The build script downloads
   the player and dependencies listed in `scripts/kindle-spotify-dependencies.json`
   and verifies their hashes. Build output is ignored by Git.
3. Connect the Kindle by USB and run:

   ```sh
   npm run music:install -- --speaker-mac AA:BB:CC:DD:EE:FF --volume /Volumes/Kindle
   ```

   Replace the example address with your paired speaker's address. Subsequent
   installs preserve the saved address if the option is omitted. No pairing is
   created by the installer. An optional `--debug-host` accepts your desktop IPv4.
4. Eject the Kindle, keep Wi-Fi on, and open KUAL → Kindle Spotify → Start.
   The dashboard launchers also start the optional player when installed.
5. Authorize the player using its own login flow. Set `SPOTIFY_DEVICE_NAME` to its
   displayed device name for widget control.

The installer preserves private session data, generates a local control token,
and stores speaker/debug settings in the device's private folder. Updates must
preserve that folder. The runtime and player binary are built locally rather than
committed. Refresh the download manifest when replacing an expired player build.

Diagnostic commands use `python3 scripts/kindle-spotify-control.py --help`.
Device logs can contain listening history or network information; keep them local.
`npm run check` includes mocked Web API and native view tests. The control-service
and PCM-mixer integration tests require Linux and skip on other hosts.
