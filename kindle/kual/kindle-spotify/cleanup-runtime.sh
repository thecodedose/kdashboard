#!/bin/sh
# Terminate only processes executing this app's private binaries or FIFO sink.
# Includes Soloist's crash handler, which can keep the old 29 MB inode mapped.
RUN=/var/tmp/kindle-spotify
owned() {
    EXE=$(readlink "$1/exe" 2>/dev/null)
    case "$EXE" in
        "$RUN/soloist"|"$RUN/soloist (deleted)"|"$RUN/spotify-control"|"$RUN/spotify-control (deleted)"|"$RUN/spotify-mixer"|"$RUN/spotify-mixer (deleted)") return 0 ;;
        /usr/bin/gst-launch-0.10)
            grep -Fq -- 'name=kindle_spotify_sink' "$1/cmdline" 2>/dev/null && return 0
            grep -Fq -- "$RUN/audio.fifo" "$1/cmdline" 2>/dev/null && return 0 ;;
    esac
    return 1
}
for SIGNAL in TERM KILL; do
    for PROC in /proc/[0-9]*; do
        [ "${PROC##*/}" != "$$" ] || continue
        owned "$PROC" || continue
        kill "-$SIGNAL" "${PROC##*/}" 2>/dev/null || true
    done
    [ "$SIGNAL" != TERM ] || sleep 1
done
