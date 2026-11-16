#!/bin/sh
BASE=/mnt/us/extensions/kindle-spotify
RUN=/var/tmp/kindle-spotify
umask 077
ulimit -c 0
message() { [ "${1:-}" = quiet ] || eips 1 3 "$2" >/dev/null 2>&1 || true; }
exec >"$BASE/private/startup.log" 2>&1
echo "Starting Kindle Spotify $(date -u)"
if [ -r "$RUN/supervisor.pid" ]; then
    PID=$(cat "$RUN/supervisor.pid")
    case "$PID" in ''|*[!0-9]*) ;; *)
        if kill -0 "$PID" 2>/dev/null && grep -q 'kindle-spotify/daemon.sh' "/proc/$PID/cmdline"; then
            message "${1:-}" "Spotify is already running."
            exit 0
        fi ;;
    esac
fi
mkdir -p "$RUN/lib" "$BASE/private/state" "$BASE/private/cache"
if ! mkdir "$RUN/start.lock" 2>/dev/null; then
    message "${1:-}" "Spotify is starting."
    exit 0
fi
trap 'rmdir "$RUN/start.lock" 2>/dev/null || true' EXIT
/bin/sh "$BASE/cleanup-runtime.sh"
# Free old files before copying; /var is a 64 MB RAM filesystem.
rm -f "$RUN/soloist" "$RUN/soloist.new"
rm -rf "$RUN/lib"
mkdir -p "$RUN/lib"
cp "$BASE/bin/soloist" "$RUN/soloist.new" || exit 1
mv "$RUN/soloist.new" "$RUN/soloist" || exit 1
cp "$BASE/bin/spotify-control" "$RUN/spotify-control" || exit 1
cp "$BASE/bin/spotify-mixer" "$RUN/spotify-mixer" || exit 1
cp "$BASE"/lib/* "$RUN/lib/" || exit 1
cp "$BASE/alsa.conf" "$RUN/alsa.conf" || exit 1
chmod 700 "$RUN/soloist" "$RUN/spotify-control" "$RUN/spotify-mixer" "$RUN/lib/ld-linux-armhf.so.3"
df -h /var/tmp
rm -f "$RUN/stop" "$RUN/restart" "$RUN/sink-test" "$RUN/sink-mode" "$RUN/reconnect-speaker"
[ ! -r "$BASE/private/sink.mode" ] || cp "$BASE/private/sink.mode" "$RUN/sink-mode"
lipc-set-prop com.lab126.cmd wirelessEnable 1 >/dev/null 2>&1 || true
lipc-set-prop com.lab126.powerd preventScreenSaver 1 >/dev/null 2>&1 || true
# Keep the native Kindle Bluetooth service in charge of pairing.
lipc-set-prop -s com.lab126.btfd BTenable 0:1 >/dev/null 2>&1 || true
SPOTIFY_DEBUG_LAN=1 nohup /bin/sh "$BASE/daemon.sh" </dev/null >"$RUN/supervisor.log" 2>&1 &
echo "$!" > "$RUN/supervisor.pid"
echo "Supervisor pid=$!"
sleep 2
message "${1:-}" "Spotify starting. Please wait 15 seconds."
