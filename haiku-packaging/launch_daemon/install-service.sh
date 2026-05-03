#!/bin/sh
# install-service.sh — Install act_runner as a Haiku launch_daemon service
#
# Usage:
#   ./install-service.sh                    # install from build/act_runner
#   ./install-service.sh /path/to/act_runner
#
# After installation the daemon starts automatically at login and is
# restarted if it exits unexpectedly.
#
# NOTE: This script does NOT send SIGHUP to launch_daemon (that kills it on
# Haiku).  The service activates at the next login or when started manually
# with: launch_roster start x-vnd.act-runner

set -e

BINARY="${1:-$(dirname "$0")/../build/act_runner}"
BINARY="$(realpath "$BINARY")"

if [ ! -x "$BINARY" ]; then
    echo "ERROR: act_runner binary not found at $BINARY"
    echo "Build it first:  cd haiku-act-runner && mkdir build && cd build && cmake .. && make"
    exit 1
fi

echo "Installing act_runner from $BINARY"

# ── 1. Copy binary ──────────────────────────────────────────────────────────
INSTALL_DIR="$HOME/config/non-packaged/bin"
mkdir -p "$INSTALL_DIR"
cp "$BINARY" "$INSTALL_DIR/act_runner"
chmod 755 "$INSTALL_DIR/act_runner"
echo "  Binary installed to $INSTALL_DIR/act_runner"

# ── 2. Create settings directory ────────────────────────────────────────────
SETTINGS_DIR="$HOME/config/settings/act_runner"
mkdir -p "$SETTINGS_DIR"
echo "  Settings directory: $SETTINGS_DIR"

# ── 3. Check registration ────────────────────────────────────────────────────
if [ ! -f "$SETTINGS_DIR/config.yaml" ]; then
    echo ""
    echo "  ⚠  Runner is not registered yet."
    echo "     Run the following before starting the service:"
    echo ""
    echo "     $INSTALL_DIR/act_runner register \\"
    echo "       --url   https://your.gitea.example \\"
    echo "       --token <registration-token> \\"
    echo "       --name  $(hostname) \\"
    echo "       --labels haiku:host"
    echo ""
fi

# ── 4. Install launch_daemon descriptor ─────────────────────────────────────
LAUNCH_DIR="$HOME/config/non-packaged/data/launch"
mkdir -p "$LAUNCH_DIR"
LAUNCH_SRC="$(dirname "$0")/act_runner.launch"

# Update the binary path in the descriptor to the actual installed path
sed "s|/boot/home/config/non-packaged/bin/act_runner|$INSTALL_DIR/act_runner|g" \
    "$LAUNCH_SRC" > "$LAUNCH_DIR/act_runner.launch"
echo "  Service descriptor installed to $LAUNCH_DIR/act_runner.launch"
echo "  (activates at next login)"

# ── 5. Try to start immediately via launch_roster ───────────────────────────
echo ""
if [ -f "$SETTINGS_DIR/config.yaml" ]; then
    if /bin/launch_roster start x-vnd.act-runner 2>/dev/null; then
        echo "  ✓ Service started."
    else
        echo "  Service will start at next login (or run manually:"
        echo "    launch_roster start x-vnd.act-runner)"
    fi
fi

echo ""
echo "  To check status:     launch_roster info x-vnd.act-runner"
echo "  To stop:             launch_roster stop x-vnd.act-runner"
echo "  To disable at boot:  launch_roster disable x-vnd.act-runner"
echo ""
echo "Done."
