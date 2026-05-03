#!/usr/bin/env bash
# scripts/e2e_smoke.sh — end-to-end smoke test against a real Gitea server
#
# Runs on the Haiku box (or any POSIX host with git + curl).  Requires a
# Gitea server reachable over HTTP/HTTPS; see scripts/gitea_up.sh for the
# helper that spins one up in Docker.
#
# Flow:
#   1. Read gitea env from $GITEA_ENV_FILE (default scripts/.gitea-env.json)
#   2. Build act_runner if build/act_runner is missing
#   3. Create a throwaway repo on the Gitea server
#   4. Push one of scripts/workflows/*.yml to it under .gitea/workflows/
#   5. Register our runner (writes ~/config/settings/act_runner/{config,…})
#   6. Start `act_runner daemon` in the background
#   7. Poll /api/v1/repos/<owner>/<repo>/actions/runs until the run finishes
#   8. Print the final status + tail of the logs, delete the repo, stop runner
#
# Usage:
#     ./scripts/e2e_smoke.sh                    # runs hello.yml
#     ./scripts/e2e_smoke.sh cache-roundtrip    # picks scripts/workflows/cache-roundtrip.yml
#     ./scripts/e2e_smoke.sh --all              # runs every workflow in sequence
#     ./scripts/e2e_smoke.sh --keep             # do not delete the repo on exit
#
# Env overrides:
#     GITEA_ENV_FILE   path to JSON emitted by gitea_up.sh
#     BUILD_DIR        default: build (beside CMakeLists.txt)
#     RUNNER_NAME      default: haiku-smoke-$$
#     RUN_TIMEOUT      seconds to wait for a run to finish (default 180)

set -euo pipefail

# ── Paths ─────────────────────────────────────────────────────────────────

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
GITEA_ENV_FILE="${GITEA_ENV_FILE:-$HERE/.gitea-env.json}"
WORKFLOW_DIR="$HERE/workflows"

# ── Logging ───────────────────────────────────────────────────────────────

_colour() {
    # $1 = colour code, $2... = message
    local code="$1"; shift
    printf '\033[%sm%s\033[0m\n' "$code" "$*"
}
info()  { _colour '1;34' "[smoke] $*"; }
ok()    { _colour '1;32' "[smoke] $*"; }
warn()  { _colour '1;33' "[smoke] $*"; }
fail()  { _colour '1;31' "[smoke] $*" >&2; }
die()   { fail "$*"; exit 1; }

# ── Arg parsing ───────────────────────────────────────────────────────────

WORKFLOWS=()
KEEP_REPO=0
RUN_ALL=0
while (( $# > 0 )); do
    case "$1" in
        --keep)  KEEP_REPO=1 ;;
        --all)   RUN_ALL=1 ;;
        -h|--help)
            sed -n '1,30p' "$0"
            exit 0
            ;;
        *)
            WORKFLOWS+=("$1")
            ;;
    esac
    shift
done

if (( RUN_ALL )); then
    WORKFLOWS=()
    for f in "$WORKFLOW_DIR"/*.yml; do
        WORKFLOWS+=("$(basename "$f" .yml)")
    done
elif (( ${#WORKFLOWS[@]} == 0 )); then
    WORKFLOWS=(hello)
fi

# ── Read gitea env ───────────────────────────────────────────────────────

[[ -f "$GITEA_ENV_FILE" ]] || die \
    "No gitea env file at $GITEA_ENV_FILE. Run scripts/gitea_up.sh on a \
Docker host and copy the resulting .gitea-env.json over, or set \
GITEA_ENV_FILE to point at it."

# Tiny JSON extractor using python3 if present, else sed (for single-line JSON).
jget() {
    local key="$1"
    if command -v python3 >/dev/null 2>&1; then
        python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])" \
            "$GITEA_ENV_FILE" "$key"
    else
        sed -n 's/.*"'"$key"'": *"\([^"]*\)".*/\1/p' "$GITEA_ENV_FILE"
    fi
}

GITEA_URL="$(jget gitea_url)"
ADMIN_USER="$(jget admin_user)"
ADMIN_PASS="$(jget admin_pass)"
ADMIN_TOKEN="$(jget admin_token)"
REG_TOKEN="$(jget runner_registration_token)"

[[ -n "$GITEA_URL" && -n "$ADMIN_TOKEN" ]] || die "gitea env file missing required keys"

info "Gitea URL     : $GITEA_URL"
info "Admin user    : $ADMIN_USER"
info "Workflows     : ${WORKFLOWS[*]}"

# ── curl helper ──────────────────────────────────────────────────────────

GAPI() {
    # Call Gitea API with admin token.  $1=method $2=path [$3=data (JSON)]
    local method="$1" path="$2" data="${3:-}"
    local url="${GITEA_URL}${path}"
    if [[ -n "$data" ]]; then
        curl -fsS -X "$method" \
            -H "Authorization: token ${ADMIN_TOKEN}" \
            -H 'Content-Type: application/json' \
            --data "$data" \
            "$url"
    else
        curl -fsS -X "$method" \
            -H "Authorization: token ${ADMIN_TOKEN}" \
            "$url"
    fi
}

# ── Build act_runner if needed ───────────────────────────────────────────

if [[ ! -x "$BUILD_DIR/act_runner" ]]; then
    info "Building act_runner (no binary at $BUILD_DIR/act_runner)..."
    mkdir -p "$BUILD_DIR"
    (cd "$BUILD_DIR" && cmake -DCMAKE_BUILD_TYPE=Release "$ROOT" >/dev/null)
    make -C "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 2)" act_runner >/dev/null
    ok "Built $BUILD_DIR/act_runner"
fi

RUNNER_BIN="$BUILD_DIR/act_runner"
RUNNER_NAME="${RUNNER_NAME:-haiku-smoke-$$}"

# ── Cleanup: kill daemon + delete repo on exit ───────────────────────────

DAEMON_PID=""
REPO_NAME=""
SMOKE_CONFIG_DIR=""

cleanup() {
    local rc=$?
    trap - EXIT INT TERM

    if [[ -n "$DAEMON_PID" ]] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        info "Stopping runner daemon (pid $DAEMON_PID)..."
        kill -TERM "$DAEMON_PID" 2>/dev/null || true
        local t=0
        while (( t < 15 )) && kill -0 "$DAEMON_PID" 2>/dev/null; do
            sleep 1; t=$((t+1))
        done
        kill -KILL "$DAEMON_PID" 2>/dev/null || true
    fi

    if [[ -n "$REPO_NAME" ]] && (( ! KEEP_REPO )); then
        info "Deleting repo $ADMIN_USER/$REPO_NAME on Gitea..."
        GAPI DELETE "/api/v1/repos/$ADMIN_USER/$REPO_NAME" || true
    fi

    # Remove the throwaway runner registration so repeated runs don't
    # accumulate stale runners in Gitea's admin UI.
    if [[ -n "${RUNNER_UUID:-}" ]]; then
        info "Removing runner '$RUNNER_NAME' from Gitea..."
        local rid
        rid=$(curl -fsS -H "Authorization: token ${ADMIN_TOKEN}" \
               "${GITEA_URL}/api/v1/admin/runners" 2>/dev/null \
               | sed -n 's/.*"id":\([0-9]*\),"name":"'"$RUNNER_NAME"'".*/\1/p' \
               | head -1)
        [[ -n "$rid" ]] && GAPI DELETE "/api/v1/admin/runners/$rid" >/dev/null 2>&1 || true
    fi

    if [[ -n "$SMOKE_CONFIG_DIR" && -d "$SMOKE_CONFIG_DIR" ]]; then
        rm -rf "$SMOKE_CONFIG_DIR"
    fi

    exit $rc
}
trap cleanup EXIT INT TERM

# ── Create repo ──────────────────────────────────────────────────────────

REPO_NAME="smoke-$(date +%s)-$$"
info "Creating repo $ADMIN_USER/$REPO_NAME..."
GAPI POST /api/v1/user/repos \
    "{\"name\":\"$REPO_NAME\",\"private\":false,\"auto_init\":true,\"default_branch\":\"main\"}" \
    > /dev/null

# ── Register runner (uses isolated settings dir so we don't clobber user cfg) ─

SMOKE_CONFIG_DIR="$(mktemp -d /tmp/act_runner_smoke_XXXXXX)"
# act_runner uses find_directory(B_USER_SETTINGS_DIRECTORY) which honours
# the HOME env var's settings subdir.  By setting HOME=$SMOKE_CONFIG_DIR,
# the runner writes its config + state there.
export HOME="$SMOKE_CONFIG_DIR"
mkdir -p "$SMOKE_CONFIG_DIR/config/settings/act_runner"
SMOKE_CONFIG="$SMOKE_CONFIG_DIR/config/settings/act_runner/config.yaml"

info "Registering runner '$RUNNER_NAME' (settings under $SMOKE_CONFIG_DIR)..."
"$RUNNER_BIN" register \
    --url    "$GITEA_URL" \
    --token  "$REG_TOKEN" \
    --name   "$RUNNER_NAME" \
    --labels "haiku:host,haiku-latest:host,self-hosted:host" \
    --config "$SMOKE_CONFIG" \
    >/dev/null

# Bump capacity to handle matrix jobs (up to 4 parallel tasks)
sed -i 's/^capacity:.*/capacity: 4/' "$SMOKE_CONFIG" 2>/dev/null || true

ok "Runner registered."

# ── Start daemon ─────────────────────────────────────────────────────────

DAEMON_LOG="$SMOKE_CONFIG_DIR/daemon.log"
info "Starting daemon (log: $DAEMON_LOG)"
# Note: redirect only stdout to the log file; keep stderr on the terminal
# (or /dev/null).  On Haiku, redirecting both stdout and stderr to the same
# regular file can cause curl's internal signal handling to kill the process
# before the first FetchTask.  The NOSIGNAL workaround is in RunnerClient.cpp
# but an open PTY-like stderr is the safest option for the daemon.
"$RUNNER_BIN" daemon --config "$SMOKE_CONFIG" >"$DAEMON_LOG" 2>>"$DAEMON_LOG" &
DAEMON_PID=$!
sleep 2
kill -0 "$DAEMON_PID" 2>/dev/null \
    || { tail -20 "$DAEMON_LOG"; die "daemon died immediately"; }
ok "Daemon up (pid $DAEMON_PID)."

# Wait until the runner appears online in Gitea (status == idle).
info "Waiting for runner to register as online..."
for _ in $(seq 1 30); do
    if curl -fsS -H "Authorization: token ${ADMIN_TOKEN}" \
        "${GITEA_URL}/api/v1/admin/runners" 2>/dev/null \
        | grep -q "\"name\":\"$RUNNER_NAME\""; then
        ok "Runner is visible to Gitea."
        break
    fi
    sleep 1
done

# ── Run each workflow in sequence ────────────────────────────────────────

ALL_PASSED=1
for WF in "${WORKFLOWS[@]}"; do
    WF_FILE="$WORKFLOW_DIR/$WF.yml"
    [[ -f "$WF_FILE" ]] || { fail "No such workflow: $WF_FILE"; ALL_PASSED=0; continue; }

    info ""
    info "══════════════════════════════════════════════════════════════════"
    info "Workflow: $WF"
    info "══════════════════════════════════════════════════════════════════"

    # ── Push .gitea/workflows/$WF.yml via the contents API ────────────────
    # (avoids the need for a git client on Haiku)
    WF_CONTENT_B64="$(base64 -w0 < "$WF_FILE" 2>/dev/null || base64 < "$WF_FILE" | tr -d '\n')"
    WF_PATH=".gitea/workflows/$WF.yml"
    WF_COMMIT_MSG="smoke: add $WF workflow"

    # First push (create).  If the file already exists from an earlier run,
    # the API returns 409 and we fall through to the update branch.
    CREATE_RESP="$(curl -fsS \
        -H "Authorization: token ${ADMIN_TOKEN}" \
        -H 'Content-Type: application/json' \
        -X POST "${GITEA_URL}/api/v1/repos/${ADMIN_USER}/${REPO_NAME}/contents/${WF_PATH}" \
        -d "{\"content\":\"$WF_CONTENT_B64\",\"message\":\"$WF_COMMIT_MSG\",\"branch\":\"main\"}" \
        2>&1)" || {
            warn "create failed, trying update..."
            # fetch sha
            SHA="$(curl -fsS -H "Authorization: token ${ADMIN_TOKEN}" \
                "${GITEA_URL}/api/v1/repos/${ADMIN_USER}/${REPO_NAME}/contents/${WF_PATH}" \
                | sed -n 's/.*"sha":"\([^"]*\)".*/\1/p' | head -1)"
            curl -fsS -H "Authorization: token ${ADMIN_TOKEN}" \
                -H 'Content-Type: application/json' \
                -X PUT "${GITEA_URL}/api/v1/repos/${ADMIN_USER}/${REPO_NAME}/contents/${WF_PATH}" \
                -d "{\"content\":\"$WF_CONTENT_B64\",\"message\":\"$WF_COMMIT_MSG\",\"branch\":\"main\",\"sha\":\"$SHA\"}" \
                >/dev/null
        }
    ok "Pushed $WF_PATH."

    # ── Poll for the run ─────────────────────────────────────────────────
    RUN_TIMEOUT="${RUN_TIMEOUT:-180}"
    info "Waiting up to ${RUN_TIMEOUT}s for run to complete..."

    RUN_ID=""
    STATUS=""
    CONCLUSION=""
    START_TS=$(date +%s)
    while :; do
        NOW=$(date +%s)
        (( NOW - START_TS > RUN_TIMEOUT )) && { fail "timeout"; break; }

        # ── Daemon health check: restart if daemon process has exited ────────
        # Check for actual process death first (immediate restart), then fall
        # back to log-silence detection for the SIGKILLTHR crash case where
        # the process exits without logging.
        if [[ -n "$DAEMON_PID" ]]; then
            if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
                warn "Daemon (pid $DAEMON_PID) exited — restarting..."
                "$RUNNER_BIN" daemon --config "$SMOKE_CONFIG" >>"$DAEMON_LOG" 2>&1 &
                DAEMON_PID=$!
                sleep 1
                ok "Daemon restarted (pid $DAEMON_PID)."
            elif [[ -f "$DAEMON_LOG" ]]; then
                LOG_MOD=$(stat -c %Y "$DAEMON_LOG" 2>/dev/null || echo 0)
                LOG_AGE=$(( NOW - LOG_MOD ))
                # NOTE: FetchTask long-polls for ~35s when queue is empty — set
                # threshold well above 35s to avoid false restarts.
                if (( LOG_AGE > 80 )); then
                    LAST_LINES="$(tail -3 "$DAEMON_LOG" 2>/dev/null)"
                    if ! echo "$LAST_LINES" | grep -q "Shutdown complete"; then
                        warn "Daemon silent for ${LOG_AGE}s — restarting..."
                        kill "$DAEMON_PID" 2>/dev/null || true
                        sleep 1
                        "$RUNNER_BIN" daemon --config "$SMOKE_CONFIG" >>"$DAEMON_LOG" 2>&1 &
                        DAEMON_PID=$!
                        sleep 1
                        ok "Daemon restarted (pid $DAEMON_PID)."
                    fi
                fi
            fi
        fi

        RUNS_JSON="$(curl -fsS -H "Authorization: token ${ADMIN_TOKEN}" \
            "${GITEA_URL}/api/v1/repos/${ADMIN_USER}/${REPO_NAME}/actions/runs?limit=5" \
            2>/dev/null || echo '{}')"

        # Pick the newest run for this workflow file.
        RUN_ID="$(printf '%s' "$RUNS_JSON" \
            | python3 -c "
import json,sys
try:
    j=json.load(sys.stdin)
    for r in j.get('workflow_runs', []):
        if r.get('path','').split('@')[0].endswith('${WF}.yml') or r.get('path','').endswith('${WF}.yml'):
            print(r['id']); break
except: pass
" 2>/dev/null)"

        if [[ -n "$RUN_ID" ]]; then
            INFO_JSON="$(curl -fsS -H "Authorization: token ${ADMIN_TOKEN}" \
                "${GITEA_URL}/api/v1/repos/${ADMIN_USER}/${REPO_NAME}/actions/runs/${RUN_ID}" \
                2>/dev/null || echo '{}')"
            STATUS="$(printf '%s' "$INFO_JSON" \
                | python3 -c "import json,sys; print(json.load(sys.stdin).get('status',''))" \
                2>/dev/null)"
            CONCLUSION="$(printf '%s' "$INFO_JSON" \
                | python3 -c "import json,sys; print(json.load(sys.stdin).get('conclusion','') or '')" \
                2>/dev/null)"

            printf '\r  run #%s  status=%-10s conclusion=%-10s  [%ds]' \
                "$RUN_ID" "$STATUS" "$CONCLUSION" "$((NOW - START_TS))"

            if [[ "$STATUS" == "completed" ]]; then
                echo ""
                break
            fi
        else
            printf '\r  (no run yet)  [%ds]' "$((NOW - START_TS))"
        fi
        sleep 3
    done

    # ── Verdict ──────────────────────────────────────────────────────────
    if [[ "$CONCLUSION" == "success" ]]; then
        ok "Workflow '$WF' succeeded (run #$RUN_ID)."
    else
        fail "Workflow '$WF' failed: status=$STATUS conclusion=$CONCLUSION"
        ALL_PASSED=0
        # Dump last 50 lines of daemon log for diagnostics.
        warn "── last daemon log lines ───────────────────────"
        tail -50 "$DAEMON_LOG" || true
        warn "─────────────────────────────────────────────────"
    fi
done

if (( ALL_PASSED )); then
    ok "════════════════════════════════════════"
    ok "All ${#WORKFLOWS[@]} workflow(s) passed."
    ok "════════════════════════════════════════"
    exit 0
else
    fail "One or more workflows failed."
    exit 1
fi
