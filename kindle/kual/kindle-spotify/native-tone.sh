#!/bin/sh
# Send known PCM without Spotify, preserving the continuous A2DP keepalive.
BASE=/mnt/us/extensions/kindle-spotify
RUN=/var/tmp/kindle-spotify
[ "$1" = clocked ] || { echo 'Native keepalive test requires clocked mode.'; exit 2; }
lipc-set-prop com.lab126.audiomgrd setFocus Music 2>&1 || true
cat "$BASE/test-tone.raw" > "$RUN/audio.fifo"
