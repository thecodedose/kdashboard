#!/bin/sh
BASE=/mnt/us/extensions/kindle-spotify
RUN=/var/tmp/kindle-spotify
PLAYER=
SINK=
CONTROL=
LAST_BT=
stop_sink() {
    [ -n "$SINK" ] || return 0
    kill "$SINK" 2>/dev/null || true
    sleep 1
    OWNER=$(awk '$1 == "PPid:" { print $2 }' "/proc/$SINK/status" 2>/dev/null)
    [ "$OWNER" != "$$" ] || kill -KILL "$SINK" 2>/dev/null || true
    wait "$SINK" 2>/dev/null || true
    SINK=
}
cleanup() {
    trap - EXIT
    trap '' INT TERM HUP
    /bin/sh "$BASE/network.sh" stop 2>/dev/null || true
    for pid in "$PLAYER" "$SINK" "$CONTROL"; do
        [ -z "$pid" ] || kill "$pid" 2>/dev/null || true
    done
    sleep 1
    for pid in "$PLAYER" "$SINK" "$CONTROL"; do
        [ -z "$pid" ] && continue
        OWNER=$(awk '$1 == "PPid:" { print $2 }' "/proc/$pid/status" 2>/dev/null)
        [ "$OWNER" != "$$" ] || kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    /bin/sh "$BASE/collect.sh" quiet 2>/dev/null || true
    rm -f "$RUN/supervisor.pid"
}
trap 'cleanup; exit 0' INT TERM HUP EXIT
rm -f "$RUN/audio.fifo"
mkfifo -m 600 "$RUN/audio.fifo" || exit 1
# A keeper descriptor prevents EOF between Spotify streams and tracks.
exec 9<> "$RUN/audio.fifo"
"$RUN/spotify-control" --server >"$RUN/control.log" 2>&1 &
CONTROL=$!
echo "Control service started; network addresses:"
ip -4 addr show wlan0 2>/dev/null || true
ITERATION=0
RESET_BT=1
while [ ! -e "$RUN/stop" ]; do
    ITERATION=$((ITERATION + 1))
    # Wi-Fi and Spotify's listener may become ready after the first refresh.
    [ $((ITERATION % 3)) -ne 1 ] || /bin/sh "$BASE/network.sh" refresh >>"$RUN/supervisor.log" 2>&1 || true
    [ "$ITERATION" -ne 6 ] || /bin/sh "$BASE/collect.sh" quiet 2>/dev/null || true
    if [ -e "$RUN/restart" ]; then
        rm -f "$RUN/restart"
        if [ -n "$PLAYER" ]; then
            kill "$PLAYER" 2>/dev/null || true
            sleep 1
            OWNER=$(awk '$1 == "PPid:" { print $2 }' "/proc/$PLAYER/status" 2>/dev/null)
            [ "$OWNER" != "$$" ] || kill -KILL "$PLAYER" 2>/dev/null || true
            wait "$PLAYER" 2>/dev/null || true
        fi
        PLAYER=
    fi
    if [ -e "$RUN/restart-sink" ]; then
        rm -f "$RUN/restart-sink"
        stop_sink
        RESET_BT=1
    fi
    if [ -e "$RUN/reconnect-speaker" ]; then
        rm -f "$RUN/reconnect-speaker"
        stop_sink
        RESET_BT=1
    fi
    if [ "$RESET_BT" = 1 ]; then
        /bin/sh "$BASE/reconnect-speaker.sh" >>"$RUN/supervisor.log" 2>&1 || true
        RESET_BT=0
    fi
    BT_NAME=$(lipc-get-prop com.lab126.btfd BTconnectedDevName 2>/dev/null)
    if [ "$BT_NAME" != "$LAST_BT" ]; then
        echo "Bluetooth output connection changed."
        stop_sink
        LAST_BT=$BT_NAME
    fi
    if [ ! -e "$RUN/sink-test" ] && [ -n "$BT_NAME" ] && { [ -z "$SINK" ] || ! kill -0 "$SINK" 2>/dev/null; }; then
        SINK_MODE=$(cat "$RUN/sink-mode" 2>/dev/null)
        case "$SINK_MODE" in clocked|unclocked|kinamp|kinamp-clocked) ;; *) SINK_MODE=clocked ;; esac
        lipc-set-prop com.lab126.audiomgrd setFocus Music >>"$RUN/supervisor.log" 2>&1 || true
        "$RUN/spotify-mixer" "$SINK_MODE" >"$RUN/sink.log" 2>&1 &
        SINK=$!
    fi
    if [ -z "$PLAYER" ] || ! kill -0 "$PLAYER" 2>/dev/null; then
        if [ -n "$PLAYER" ]; then
            wait "$PLAYER"; RESULT=$?
            echo "Player exited: $RESULT"
            [ "$RESULT" != 10 ] || break
        fi
        "$RUN/spotify-control" --launch >"$RUN/player.log" 2>&1 &
        PLAYER=$!
        echo "$PLAYER" > "$RUN/player.pid"
        sleep 1
        /bin/sh "$BASE/network.sh" refresh >>"$RUN/supervisor.log" 2>&1 || true
    fi
    sleep 2
done
cleanup
