#!/bin/sh
RUN=/var/tmp/kindle-spotify
BASE=/mnt/us/extensions/kindle-spotify
touch "$RUN/stop" 2>/dev/null || true
if [ -r "$RUN/supervisor.pid" ]; then
    PID=$(cat "$RUN/supervisor.pid")
    case "$PID" in ''|*[!0-9]*) ;; *)
        if grep -q 'kindle-spotify/daemon.sh' "/proc/$PID/cmdline" 2>/dev/null; then
            kill "$PID" 2>/dev/null || true
            COUNT=0
            while kill -0 "$PID" 2>/dev/null && [ "$COUNT" -lt 8 ]; do
                sleep 1; COUNT=$((COUNT + 1))
            done
        fi ;;
    esac
fi
/bin/sh "$BASE/network.sh" stop 2>/dev/null || true
/bin/sh "$BASE/cleanup-runtime.sh"
eips 1 3 "Kindle Spotify player stopped." >/dev/null 2>&1 || true
