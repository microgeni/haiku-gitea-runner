#!/usr/bin/env bash
# scripts/gitea_up.sh — spin up a throwaway Gitea instance via Docker
#
# Runs on any machine that has Docker (or Podman) and network access.
# The resulting Gitea server is reachable from the Haiku box over the LAN.
#
# What it does:
#   1. Starts a Gitea container on the given host port (default 3000).
#   2. Waits for Gitea to answer /api/v1/version.
#   3. Runs the first-time install via /install (admin user = "smoketest").
#   4. Creates a personal-access token for the admin user.
#   5. Enables Gitea Actions globally (via gitea.ini edit inside the container).
#   6. Emits a JSON file with {gitea_url, admin_user, admin_pass, admin_token,
#      runner_registration_token} that scripts/e2e_smoke.sh consumes.
#
# Usage:
#     GITEA_PORT=3000 GITEA_HOST_IP=192.168.1.10 ./scripts/gitea_up.sh
#     # writes ./scripts/.gitea-env.json
#
# Tear down:
#     ./scripts/gitea_up.sh down
#
# Environment variables:
#     GITEA_IMAGE      — image tag (default: gitea/gitea:1.22)
#     GITEA_PORT       — host port to expose (default: 3000)
#     GITEA_HOST_IP    — IP that the Haiku box will use to reach us (required
#                        for cross-machine tests; default: hostname -I first IP)
#     GITEA_DATA_DIR   — host bind mount (default: /tmp/gitea-smoke-data)
#     DOCKER           — docker|podman (default: whichever is on PATH)
#     GITEA_ADMIN      — admin username (default: smoketest)
#     GITEA_PASS       — admin password (default: SmokeTest-1234!)

set -euo pipefail

# ── Config ────────────────────────────────────────────────────────────────

GITEA_IMAGE="${GITEA_IMAGE:-gitea/gitea:1.22}"
GITEA_PORT="${GITEA_PORT:-3000}"
GITEA_DATA_DIR="${GITEA_DATA_DIR:-/tmp/gitea-smoke-data}"
GITEA_ADMIN="${GITEA_ADMIN:-smoketest}"
GITEA_PASS="${GITEA_PASS:-SmokeTest-1234!}"
GITEA_EMAIL="${GITEA_EMAIL:-smoketest@example.local}"
CONTAINER_NAME="${CONTAINER_NAME:-gitea-smoke}"

if [[ -z "${DOCKER:-}" ]]; then
    if command -v docker >/dev/null 2>&1; then
        DOCKER=docker
    elif command -v podman >/dev/null 2>&1; then
        DOCKER=podman
    else
        echo "ERROR: need docker or podman on PATH (or set \$DOCKER)" >&2
        exit 1
    fi
fi

# The Haiku runner connects to us by IP.  Guess the primary LAN IP.
if [[ -z "${GITEA_HOST_IP:-}" ]]; then
    if command -v hostname >/dev/null && hostname -I >/dev/null 2>&1; then
        GITEA_HOST_IP="$(hostname -I | awk '{print $1}')"
    else
        # macOS fallback
        GITEA_HOST_IP="$(ifconfig 2>/dev/null | awk '/inet / && $2 != "127.0.0.1" {print $2; exit}')"
    fi
fi
GITEA_HOST_IP="${GITEA_HOST_IP:-127.0.0.1}"

GITEA_URL="http://${GITEA_HOST_IP}:${GITEA_PORT}"
OUTFILE="$(dirname "$0")/.gitea-env.json"

# ── Helpers ───────────────────────────────────────────────────────────────

log() { printf '\033[1;34m[gitea_up]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[gitea_up ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

wait_for_http() {
    local url="$1"
    local timeout="${2:-120}"
    local t=0
    while (( t < timeout )); do
        if curl -fsS -o /dev/null "$url" 2>/dev/null; then
            return 0
        fi
        sleep 2
        t=$((t + 2))
    done
    die "timed out waiting for $url"
}

# ── Subcommand: down ──────────────────────────────────────────────────────

if [[ "${1:-up}" == "down" ]]; then
    log "Stopping container $CONTAINER_NAME"
    $DOCKER rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
    log "Removing data dir $GITEA_DATA_DIR"
    rm -rf "$GITEA_DATA_DIR" || true
    rm -f "$OUTFILE"
    log "Done."
    exit 0
fi

# ── up ────────────────────────────────────────────────────────────────────

log "Using Docker engine: $DOCKER"
log "Gitea image        : $GITEA_IMAGE"
log "Gitea URL          : $GITEA_URL"
log "Admin user/pass    : $GITEA_ADMIN / $GITEA_PASS"
log "Data dir           : $GITEA_DATA_DIR"

# Stop any prior instance.
$DOCKER rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
mkdir -p "$GITEA_DATA_DIR"

log "Starting Gitea container..."
$DOCKER run -d \
    --name "$CONTAINER_NAME" \
    -p "${GITEA_PORT}:3000" \
    -v "${GITEA_DATA_DIR}:/data" \
    -e USER_UID=1000 -e USER_GID=1000 \
    -e GITEA__server__ROOT_URL="${GITEA_URL}/" \
    -e GITEA__actions__ENABLED=true \
    -e GITEA__service__DISABLE_REGISTRATION=false \
    "$GITEA_IMAGE" >/dev/null

log "Waiting for /api/v1/version (up to 120 s)..."
wait_for_http "${GITEA_URL}/api/v1/version" 120

# ── First-time install ───────────────────────────────────────────────────
# Gitea 1.22 auto-installs when GITEA__* env vars are set — but we still need
# to create the admin user.  Use the CLI inside the container.

log "Creating admin user '$GITEA_ADMIN'..."
# Retry: the internal services can take a few seconds after /version responds.
for i in 1 2 3 4 5; do
    if $DOCKER exec -u git "$CONTAINER_NAME" gitea admin user create \
        --username "$GITEA_ADMIN" \
        --password "$GITEA_PASS" \
        --email    "$GITEA_EMAIL" \
        --admin --must-change-password=false 2>&1 | tee /tmp/gitea-admin.out; then
        break
    fi
    if grep -q "already exists" /tmp/gitea-admin.out; then
        log "  admin user already exists — OK"
        break
    fi
    log "  attempt $i failed, retrying in 5 s..."
    sleep 5
    (( i == 5 )) && die "failed to create admin user"
done

# ── Create access token ──────────────────────────────────────────────────

log "Creating personal access token..."
ADMIN_TOKEN="$(
    curl -fsS -u "${GITEA_ADMIN}:${GITEA_PASS}" \
        -H 'Content-Type: application/json' \
        -X POST "${GITEA_URL}/api/v1/users/${GITEA_ADMIN}/tokens" \
        -d "{\"name\":\"smoke-$(date +%s)\",\"scopes\":[\"write:repository\",\"write:user\",\"write:admin\"]}" \
    | sed -n 's/.*"sha1":"\([^"]*\)".*/\1/p'
)"

[[ -n "$ADMIN_TOKEN" ]] || die "failed to extract token from response"
log "  got token ${ADMIN_TOKEN:0:8}..."

# ── Fetch runner registration token (global) ─────────────────────────────

log "Fetching runner registration token..."
RUNNER_TOKEN="$(
    curl -fsS -H "Authorization: token ${ADMIN_TOKEN}" \
        "${GITEA_URL}/api/v1/admin/runners/registration-token" \
    | sed -n 's/.*"token":"\([^"]*\)".*/\1/p'
)"

[[ -n "$RUNNER_TOKEN" ]] || die "failed to fetch runner registration token"
log "  got runner token ${RUNNER_TOKEN:0:8}..."

# ── Write env file ───────────────────────────────────────────────────────

cat > "$OUTFILE" <<EOF
{
  "gitea_url":                  "$GITEA_URL",
  "admin_user":                 "$GITEA_ADMIN",
  "admin_pass":                 "$GITEA_PASS",
  "admin_token":                "$ADMIN_TOKEN",
  "runner_registration_token":  "$RUNNER_TOKEN",
  "container":                  "$CONTAINER_NAME"
}
EOF
log "Wrote $OUTFILE"

cat <<EOF

┌────────────────────────────────────────────────────────────────────┐
│ Gitea is up!                                                        │
│                                                                     │
│   URL        : $GITEA_URL
│   UI login   : $GITEA_ADMIN / $GITEA_PASS
│   Admin PAT  : ${ADMIN_TOKEN:0:8}…  (full in $OUTFILE)
│   Runner reg : ${RUNNER_TOKEN:0:8}…  (ditto)
│                                                                     │
│ On the Haiku box run:                                               │
│   scp $OUTFILE haiku:~/haiku-act-runner/scripts/
│   cd ~/haiku-act-runner && ./scripts/e2e_smoke.sh                   │
│                                                                     │
│ To tear down:                                                       │
│   $0 down                                                           │
└────────────────────────────────────────────────────────────────────┘
EOF
