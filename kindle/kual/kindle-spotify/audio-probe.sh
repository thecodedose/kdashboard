#!/bin/sh
# Read-only diagnostics for the native Kindle audio path; no key or argv dumps.
RUN=/var/tmp/kindle-spotify
echo '=== Bluetooth ==='
for prop in BTstate BTconnectedDevName btEnabled btPowerState; do
    echo "$prop:"
    lipc-get-prop com.lab126.btfd "$prop" 2>&1 || true
done
echo '=== Audio manager properties ==='
for prop in audioOutputConnected audioCurrentOutput isStarted speakerVolume; do
    echo "$prop:"
    lipc-get-prop com.lab126.audiomgrd "$prop" 2>&1 || true
done
echo '=== Audio property names ==='
lipc-probe -a 2>/dev/null | awk '/^[^ \t]/ { selected = ($0 == "com.lab126.audiomgrd" || $0 == "com.lab126.btfd" || $0 == "com.lab126.playermgr") } selected'
echo '=== Audio processes ==='
for proc in /proc/[0-9]*; do
    name=$(cat "$proc/comm" 2>/dev/null)
    case "$name" in audiomgrd|playermgr|gst-launch-0.10|bluetoothd|btfd)
        echo "${proc##*/}: $name"
        cat "$proc/wchan" 2>/dev/null
        grep -E 'mixer|audio|a2dp' "$proc/maps" 2>/dev/null || true
        ls -l "$proc/fd" 2>/dev/null | tail -15
        ;;
    esac
done
echo '=== Bluetooth audio sockets ==='
grep -Ei 'a2dp|audio|mixer' /proc/net/unix 2>/dev/null || true
echo '=== Sink logs ==='
tail -c 12000 "$RUN/sink.log" 2>/dev/null || true
echo '=== Continuous mixer counters ==='
cat "$RUN/mixer.stats" 2>/dev/null || true
echo '=== Native audio log paths ==='
ls -ld /var/log/messages /var/local/log/messages /var/tmp/audiomgrd.err 2>/dev/null
echo '=== Native audio errors ==='
tail -c 12000 /var/tmp/audiomgrd.err 2>/dev/null || true
echo '=== Audio kernel messages ==='
dmesg 2>/dev/null | grep -Ei 'audiomgr|mixersink|mixerAPI|a2dp|bluetooth|speakerVolume' | tail -60
