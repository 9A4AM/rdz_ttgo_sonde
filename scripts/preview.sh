#!/usr/bin/env bash
#
# Start / restart / stop the local web-UI preview server (scripts/preview_server.py).
#
# Re-running always cleans up the previous instance first, so you never hit a stale
# process or an "address already in use" error when reopening the preview.
#
# Usage:
#   scripts/preview.sh [PORT]     start (or restart) the preview server  (default port 8099)
#   scripts/preview.sh stop       stop a running preview server
#   scripts/preview.sh restart    same as start
#
# Env:
#   PYTHON   python interpreter to use (default: python3)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER="$SCRIPT_DIR/preview_server.py"
PY="${PYTHON:-python3}"

stop_running() {
  # Kill any previous preview instance. pkill exits 1 when nothing matched; that is
  # not an error here, so swallow it.
  if pkill -f "preview_server.py" 2>/dev/null; then
    # Give the old process a moment to release the listening socket.
    sleep 1
  fi
}

cmd="${1:-start}"
case "$cmd" in
  stop)
    stop_running
    echo "preview server stopped"
    exit 0
    ;;
  restart|start)
    PORT="${2:-8099}"
    ;;
  *)
    # First arg is a port number (e.g. `preview.sh 9000`).
    PORT="$cmd"
    ;;
esac

PORT="${PORT:-8099}"

stop_running

echo "Starting preview server on http://0.0.0.0:${PORT} (Ctrl-C to stop) ..."
exec "$PY" "$SERVER" --port "$PORT"
