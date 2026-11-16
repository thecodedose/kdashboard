#!/bin/sh
BASE=/mnt/us/extensions/kindle-spotify
MAC=$(cat "$BASE/private/speaker.mac" 2>/dev/null)
echo "$MAC" | grep -Eq '^([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}$' || exit 1
# Re-arm A2DP after a stale/idle route; reuse the existing trusted pairing.
lipc-set-prop -s com.lab126.btfd Disconnect "$MAC" 2>/dev/null || true
sleep 1
lipc-set-prop -s com.lab126.btfd Connect "$MAC" 2>&1 || exit 1
for attempt in 1 2 3 4 5 6 7 8; do
    if [ "$(lipc-get-prop com.lab126.audiomgrd audioOutputConnected 2>/dev/null)" = 1 ]; then
        echo 'Bluetooth speaker audio route reconnected.'
        exit 0
    fi
    sleep 1
done
echo 'Bluetooth speaker is unavailable; keep its existing Kindle pairing.'
exit 1
