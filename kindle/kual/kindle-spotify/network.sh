#!/bin/sh
# App-specific, runtime-only firewall exceptions. No default policy changes.
BASE=/mnt/us/extensions/kindle-spotify
RUN=/var/tmp/kindle-spotify
CHAIN=KD_SPOTIFY
if [ "${1:-}" = stop ]; then
    iptables -D INPUT -i wlan0 -j "$CHAIN" 2>/dev/null || true
    iptables -F "$CHAIN" 2>/dev/null || true
    iptables -X "$CHAIN" 2>/dev/null || true
    exit 0
fi
CIDR=$(ip -o -4 addr show wlan0 2>/dev/null | awk '{print $4; exit}')
case "$CIDR" in ''|*[!0-9./]*) exit 1 ;; esac
iptables -N "$CHAIN" 2>/dev/null || true
if ! iptables -S INPUT | grep -Fq -- "-A INPUT -i wlan0 -j $CHAIN"; then
    iptables -I INPUT 1 -i wlan0 -j "$CHAIN" || exit 1
fi
iptables -F "$CHAIN" || exit 1
# Spotify's LAN discovery and only the TCP listener owned by this player.
iptables -A "$CHAIN" -s "$CIDR" -p udp --dport 5353 -j ACCEPT
for PORT in $("$RUN/spotify-control" --ports); do
    case "$PORT" in ''|*[!0-9]*) continue ;; esac
    [ "$PORT" -ge 1024 ] && [ "$PORT" -le 65535 ] || continue
    iptables -A "$CHAIN" -s "$CIDR" -p tcp --dport "$PORT" -j ACCEPT
done
# Development controls accept the configured debug host only, and still require a private token.
if [ -r "$BASE/private/debug-host" ]; then
    DEBUG_HOST=$(cat "$BASE/private/debug-host")
    case "$DEBUG_HOST" in ''|*[!0-9.]*) ;; *)
        iptables -A "$CHAIN" -s "$DEBUG_HOST/32" -p tcp --dport 24880 -j ACCEPT
        iptables -A "$CHAIN" -s "$DEBUG_HOST/32" -p udp --dport 24880 -j ACCEPT ;;
    esac
fi
