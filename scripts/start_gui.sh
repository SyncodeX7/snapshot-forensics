#!/usr/bin/env bash
#
# Start the local web GUI and (when a desktop session is present) open it.
#
# Usage: scripts/start_gui.sh [--port N] [--host ADDR] [--no-browser]
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT=8787
HOST=127.0.0.1
BROWSER=1

while [ $# -gt 0 ]; do
    case "$1" in
        --port) PORT="$2"; shift 2 ;;
        --host) HOST="$2"; shift 2 ;;
        --no-browser) BROWSER=0; shift ;;
        -h|--help) sed -n '2,8p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$ROOT/logs" "$ROOT/out/analyzed"
URL="http://$HOST:$PORT/"

python3 "$ROOT/gui/server.py" --host "$HOST" --port "$PORT" > "$ROOT/logs/gui.log" 2>&1 &
PID=$!
cleanup() {
    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

sleep 1
if ! kill -0 "$PID" 2>/dev/null; then
    echo "GUI failed to start; see $ROOT/logs/gui.log" >&2
    cat "$ROOT/logs/gui.log" >&2 || true
    exit 1
fi

echo "snapshot GUI: $URL"
echo "log        : $ROOT/logs/gui.log"

if [ "$BROWSER" -eq 1 ] && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; }; then
    if command -v xdg-open >/dev/null 2>&1; then
        xdg-open "$URL" >/dev/null 2>&1 || true
    fi
fi

wait "$PID"
