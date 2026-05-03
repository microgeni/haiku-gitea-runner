#!/bin/sh
# install-service.sh — Install act_runner as a Haiku launch_daemon service
#
# Usage:
#   ./install-service.sh                    # use build/act_runner from project
#   ./install-service.sh /path/to/act_runner
#
# What this script does:
#   1. Copies the act_runner binary to ~/config/non-packaged/bin/
#   2. Installs the wrapper script  to ~/config/non-packaged/data/act_runner/
#   3. Installs the service descriptor to
#        ~/config/non-packaged/data/user_launch/x-vnd.act-runner
#   4. Tells you to register if config.yaml doesn't exist yet.
#   5. Tries to start the service immediately via launch_roster.
#
# NOTE: Do NOT send SIGHUP to launch_daemon — that kills it on Haiku.
#       The service activates at the next login, or immediately via:
#         launch_roster start x-vnd.act-runner

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="${1:-$SCRIPT_DIR/../build/act_runner}"
BINARY="$(realpath "$BINARY")"

if [ ! -x "$BINARY" ]; then
    echo "ERROR: act_runner binary not found at $BINARY"
    echo "Build it first:  mkdir -p build && cd build && cmake .. && make act_runner"
    exit 1
fi

echo "Installing act_runner from $BINARY"

# ── 1. Install binary ───────────────────────────────────────────────────────
BIN_DIR="$HOME/config/non-packaged/bin"
mkdir -p "$BIN_DIR"
cp "$BINARY" "$BIN_DIR/act_runner"
chmod 755 "$BIN_DIR/act_runner"
INSTALLED_BIN="$BIN_DIR/act_runner"
echo "  Binary   → $INSTALLED_BIN"

# ── 2. Install wrapper script ───────────────────────────────────────────────
DATA_DIR="$HOME/config/non-packaged/data"
WRAPPER_DIR="$DATA_DIR/act_runner"
mkdir -p "$WRAPPER_DIR"

TEMPLATE_WRAPPER="$SCRIPT_DIR/act_runner-launch.sh"
if [ ! -f "$TEMPLATE_WRAPPER" ]; then
    echo "ERROR: wrapper template not found at $TEMPLATE_WRAPPER"
    exit 1
fi

sed "s|@BIN@|$INSTALLED_BIN|g" "$TEMPLATE_WRAPPER" \
    > "$WRAPPER_DIR/act_runner-launch.sh"
chmod 755 "$WRAPPER_DIR/act_runner-launch.sh"
echo "  Wrapper  → $WRAPPER_DIR/act_runner-launch.sh"

# ── 3. Write / update run.sh ────────────────────────────────────────────────
# launch_daemon caches descriptor contents in memory for the lifetime of the
# session.  The already-loaded descriptor invokes run.sh, so we keep run.sh
# pointing at the current binary so that `launch_roster restart` picks it up
# without needing a logout.
SETTINGS_DIR="$HOME/config/settings/act_runner"
mkdir -p "$SETTINGS_DIR"
RUN_SH="$SETTINGS_DIR/run.sh"
cat > "$RUN_SH" << RUNEOF
#!/bin/sh
LOG=$SETTINGS_DIR/daemon.log
BIN=$INSTALLED_BIN
CFG=$SETTINGS_DIR/config.yaml
exec "\$BIN" daemon --config "\$CFG" >>"\$LOG" 2>&1
RUNEOF
chmod 755 "$RUN_SH"
echo "  run.sh   → $RUN_SH"

# ── 4. Install launch_daemon service descriptor ─────────────────────────────
LAUNCH_DIR="$DATA_DIR/user_launch"
mkdir -p "$LAUNCH_DIR"

TEMPLATE_LAUNCH="$SCRIPT_DIR/act_runner.launch.template"
if [ ! -f "$TEMPLATE_LAUNCH" ]; then
    echo "ERROR: launch template not found at $TEMPLATE_LAUNCH"
    exit 1
fi

sed \
    -e "s|@BIN@|$INSTALLED_BIN|g" \
    -e "s|@DATADIR@|$DATA_DIR|g" \
    "$TEMPLATE_LAUNCH" \
    > "$LAUNCH_DIR/x-vnd.act-runner"
echo "  Service  → $LAUNCH_DIR/x-vnd.act-runner"

# ── 5. Check registration ────────────────────────────────────────────────────
REGISTERED=0
if [ -f "$SETTINGS_DIR/config.yaml" ]; then
    uuid=$(grep -E '^uuid:' "$SETTINGS_DIR/config.yaml" \
           | sed 's/uuid:[[:space:]]*//' | tr -d '"'"'" | tr -d '[:space:]')
    token=$(grep -E '^runner_token:' "$SETTINGS_DIR/config.yaml" \
            | sed 's/runner_token:[[:space:]]*//' | tr -d '"'"'" | tr -d '[:space:]')
    if [ -n "$uuid" ] && [ -n "$token" ]; then
        REGISTERED=1
    fi
fi

if [ "$REGISTERED" -eq 0 ]; then
    echo ""
    echo "  ⚠  Runner is not registered yet."
    echo "     Run the following to register, then start the service:"
    echo ""
    echo "     $INSTALLED_BIN register \\"
    echo "       --url   https://your.gitea.example \\"
    echo "       --token <registration-token>       \\"
    echo "       --name  $(hostname)                \\"
    echo "       --labels haiku:host"
    echo ""
    echo "     Then:  launch_roster start x-vnd.act-runner"
    echo ""
    echo "  Installation complete — service will not start until registered."
    exit 0
fi

# ── 6. Start / restart the service ──────────────────────────────────────────
echo ""
# If already running, restart so it picks up the updated binary via run.sh.
# If not running, start it fresh.
if launch_roster info x-vnd.act-runner 2>/dev/null | grep -q "running.*yes"; then
    launch_roster restart x-vnd.act-runner 2>/dev/null
    echo "  ✓ Service restarted with new binary."
elif launch_roster start x-vnd.act-runner 2>/dev/null; then
    echo "  ✓ Service started."
else
    echo "  Service will start at next login (or run manually):"
    echo "    launch_roster start x-vnd.act-runner"
fi

echo ""
echo "  To check status:     launch_roster info x-vnd.act-runner"
echo "  To stop:             launch_roster stop x-vnd.act-runner"
echo "  To disable at boot:  launch_roster disable x-vnd.act-runner"
echo ""
echo "Done."
