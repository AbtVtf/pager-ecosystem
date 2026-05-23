#!/usr/bin/env bash
# Launch every demo app on its assigned port for local pager testing.
#
# Each app is started as a background process; PIDs are tracked in
# /tmp/pageros-apps.pids so `./launch_all.sh stop` can take them all down.
#
# Apps run with PYTHONPATH pointing at the in-repo SDK so you don't need
# to `pip install pageros` first. Logs land in /tmp/pageros-<name>.log.
#
# Port map matches marketplace/api/dev_serve.py:APP_PORTS — keep in sync.

set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SDK_PATH="$REPO/sdk/python"
LOG_DIR="${PAGEROS_APP_LOG_DIR:-/tmp}"
PIDFILE="${PAGEROS_APP_PIDFILE:-/tmp/pageros-apps.pids}"

declare -A APPS=(
  [news]=8011
  [email]=8012
  [maps]=8013
  [dm]=8014
  [drive]=8015
  [ssh]=8016
  [askai]=8017
  [stocks]=8018
  [wiki]=8019
  [translate]=8020
)

cmd="${1:-start}"

stop_all() {
  if [[ -f "$PIDFILE" ]]; then
    while read -r line; do
      pid="${line%% *}"
      [[ -z "$pid" ]] && continue
      if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
      fi
    done < "$PIDFILE"
    rm -f "$PIDFILE"
    echo "stopped tracked processes"
  else
    echo "no $PIDFILE — nothing to stop"
  fi
}

case "$cmd" in
  stop)
    stop_all
    exit 0
    ;;
  status)
    if [[ -f "$PIDFILE" ]]; then
      while read -r line; do
        pid="${line%% *}"; name="${line#* }"
        if kill -0 "$pid" 2>/dev/null; then
          echo "up    $pid  $name"
        else
          echo "down  $pid  $name"
        fi
      done < "$PIDFILE"
    else
      echo "no $PIDFILE — apps not started"
    fi
    exit 0
    ;;
  start) ;;
  *)
    echo "usage: $0 [start|stop|status]" >&2
    exit 2
    ;;
esac

# Start mode — kill any previous PIDs first so re-runs are clean.
[[ -f "$PIDFILE" ]] && stop_all

: > "$PIDFILE"
for name in "${!APPS[@]}"; do
  port="${APPS[$name]}"
  app_dir="$REPO/examples/$name"
  if [[ ! -f "$app_dir/app.py" ]]; then
    echo "skip $name (no app.py)"
    continue
  fi
  log="$LOG_DIR/pageros-$name.log"
  ( cd "$app_dir" && PYTHONPATH="$SDK_PATH" python3 app.py --host 0.0.0.0 --port "$port" \
      >"$log" 2>&1 ) &
  pid=$!
  echo "$pid $name :$port" >> "$PIDFILE"
  echo "up    $pid  $name :$port  (log: $log)"
done

cat <<EOF

All requested apps launched in the background.
  - stop them all:   $0 stop
  - status check:    $0 status
  - logs:            $LOG_DIR/pageros-<name>.log

To make the marketplace serve these URLs to the pager, also run:
  PAGEROS_DEV_LAN_IP=\$(hostname -I | awk '{print \$1}') \\
    python3 -m uvicorn dev_serve:app --host 0.0.0.0 --port 8000 \\
    --app-dir "$REPO/marketplace/api"
EOF
