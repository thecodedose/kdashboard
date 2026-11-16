#!/bin/sh
# Diagnostic only: does not stop the dashboard or issue cleaning commands.
ROOT=/mnt/us/extensions/narwal-probe
LOG=/mnt/us/documents/narwal-connection-test.txt
LOCK=/tmp/narwal-probe.lock
umask 077
if ! mkdir "$LOCK" 2>/dev/null; then
  exit 1
fi
RUN_APP="$LOCK/narwal-probe"
trap 'rm -f "$RUN_APP"; rmdir "$LOCK" 2>/dev/null' 0
NARWAL_HOST=""
if [ -f "$ROOT/config.sh" ]; then
  . "$ROOT/config.sh"
fi
{
  echo "Kindle to Narwal direct Wi-Fi connection test"
  date
  echo "No cleaning, dock, map or settings commands are sent."
  if [ -z "$NARWAL_HOST" ]; then
    echo "RESULT: Set NARWAL_HOST in extensions/narwal-probe/config.sh first."
    exit 1
  fi
  if ! ip route get "$NARWAL_HOST" 2>/dev/null; then
    echo "Network route unavailable; check Wi-Fi and airplane mode."
  fi
  # Kindle user storage may be mounted noexec, even when the file has +x.
  # Use the same copy-to-/tmp execution pattern as the native dashboard.
  if ! cp "$ROOT/bin/narwal-probe" "$RUN_APP" || ! chmod 700 "$RUN_APP"; then
    echo "RESULT: Could not prepare the executable in /tmp."
    exit 1
  fi
  "$RUN_APP" "$NARWAL_HOST" --timeout 30 --identity-file "$ROOT/identity.txt"
  result=$?
  echo "Exit code: $result (0=live status, 2=WebSocket only, 1=connection error)"
  exit "$result"
} > "$LOG" 2>&1
