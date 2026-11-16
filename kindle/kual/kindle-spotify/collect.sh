#!/bin/sh
# Private, bounded startup diagnostics; no command-line arguments or key files.
BASE=/mnt/us/extensions/kindle-spotify
RUN=/var/tmp/kindle-spotify
OUT="$BASE/private/diagnostics"
umask 077
mkdir -p "$OUT" || exit 1
for name in player.log sink.log supervisor.log control.log; do
    [ ! -f "$RUN/$name" ] || tail -c 50000 "$RUN/$name" > "$OUT/$name"
done
{
    date -u
    ip -4 addr show wlan0 2>/dev/null || true
    echo "=== memory ==="
    head -5 /proc/meminfo
    df -h /var/tmp
    du -sk "$RUN" 2>/dev/null || true
    echo "=== TCP listeners ==="
    cat /proc/net/tcp /proc/net/tcp6 2>/dev/null
    echo "=== firewall ==="
    iptables -S 2>/dev/null || true
    echo "=== player memory ==="
    if [ -r "$RUN/player.pid" ]; then
        PID=$(cat "$RUN/player.pid")
        case "$PID" in ''|*[!0-9]*) ;; *) grep -E '^(Name|State|VmRSS|VmSize|Threads):' "/proc/$PID/status" 2>/dev/null ;; esac
    fi
    echo "=== local player status ==="
    "$RUN/spotify-control" --status 2>&1
    echo "=== Bluetooth connection ==="
    lipc-get-prop com.lab126.btfd BTstate 2>/dev/null || true
    lipc-get-prop com.lab126.btfd BTconnectedDevName 2>/dev/null || true
} > "$OUT/system.txt"
[ "${1:-}" = quiet ] || eips 1 3 "Spotify diagnostics saved. Reconnect USB." >/dev/null 2>&1 || true
