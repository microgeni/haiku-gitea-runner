#!/bin/sh
# act_runner-launch.sh — launch_daemon wrapper for haiku-act-runner
#
# Installed to:  @DATADIR@/act_runner/act_runner-launch.sh
# Invoked by:    launch_daemon via the x-vnd.act-runner service descriptor
#
# Purpose
# -------
# launch_daemon's built-in `require { file … }` guard only checks that a file
# *exists* — it cannot inspect file content.  This wrapper performs a deeper
# pre-flight check:
#
#   1. config.yaml must exist.
#   2. config.yaml must contain non-empty `runner_token` and `uuid` fields,
#      which are written by `act_runner register` (and only then).
#
# If either check fails the script exits 0 so launch_daemon does NOT restart
# the service in a tight loop.  The user must register the runner first:
#
#   act_runner register \
#       --url   https://your.gitea.example \
#       --token <registration-token>
#
# Once registered, start the service manually for the first time:
#
#   launch_roster start x-vnd.act-runner
#
# Subsequent logins start it automatically.

CONFIG="$HOME/config/settings/act_runner/config.yaml"
BINARY="@BIN@"
LOGFILE="$HOME/config/settings/act_runner/daemon.log"

# ── 1. Config file must exist ──────────────────────────────────────────────
if [ ! -f "$CONFIG" ]; then
    echo "act_runner: config not found ($CONFIG) — register first." >&2
    exit 0   # exit 0 = don't let launch_daemon restart us
fi

# ── 2. runner_token must be non-empty ─────────────────────────────────────
token=$(grep -E '^runner_token:' "$CONFIG" | sed 's/runner_token:[[:space:]]*//' | tr -d '"'"'" | tr -d '[:space:]')
if [ -z "$token" ]; then
    echo "act_runner: runner_token missing in $CONFIG — register first." >&2
    exit 0
fi

# ── 3. uuid must be non-empty ─────────────────────────────────────────────
uuid=$(grep -E '^uuid:' "$CONFIG" | sed 's/uuid:[[:space:]]*//' | tr -d '"'"'" | tr -d '[:space:]')
if [ -z "$uuid" ]; then
    echo "act_runner: uuid missing in $CONFIG — register first." >&2
    exit 0
fi

# ── 4. Binary must exist and be executable ────────────────────────────────
if [ ! -x "$BINARY" ]; then
    echo "act_runner: binary not found at $BINARY" >&2
    exit 1   # exit 1 = real error, launch_daemon may retry with backoff
fi

# ── 5. All checks passed — hand off to the daemon ─────────────────────────
exec "$BINARY" daemon --config "$CONFIG" \
    >> "$LOGFILE" 2>&1
