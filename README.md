# haiku-act-runner

A native C++ port of the [Gitea Actions Runner](https://gitea.com/gitea/act_runner) for **Haiku OS**.

## Status

All phases implemented: registration, Connect-RPC polling, workflow parsing, expression evaluation, host-executor step running, composite/JS actions, local `actions/cache` HTTP server, Haiku launch_daemon integration, HPKG packaging recipe, and local workflow execution (`run` subcommand).

| Phase | Feature | Status |
|-------|---------|--------|
| 1 | Registration (HTTP REST) | ✅ Implemented |
| 2 | Connect-RPC transport (libcurl fallback) | ✅ Implemented |
| 2 | Protobuf encode/decode (hand-coded) | ✅ Implemented |
| 3 | `run:` step execution via `posix_spawn` | ✅ Implemented |
| 3 | Log streaming (UpdateLog) | ✅ Implemented |
| 3 | `$GITHUB_OUTPUT` / `$GITHUB_ENV` protocol | ✅ Implemented |
| 4 | `${{ }}` expression evaluator | ✅ Implemented |
| 4 | Context objects (github/env/runner/steps/needs/matrix) | ✅ Implemented |
| 5 | `uses:` composite + JS action support | ✅ Implemented |
| 5 | Matrix expansion (server-side pre-expansion honoured) | ✅ Implemented |
| 5 | Local `actions/cache` HTTP server | ✅ Implemented |
| 6 | Haiku `launch_daemon` service | ✅ Implemented |
| 6 | HPKG packaging recipe | ✅ Implemented |
| 6 | Local `run` subcommand (no Gitea server needed) | ✅ Implemented |
| 6 | `unregister` subcommand | ✅ Implemented |

## Dependencies

Install on Haiku:

```bash
pkgman install curl_devel yaml_cpp_devel protobuf_devel libmicrohttpd_devel
```

The nlohmann/json header is bundled in `vendor/nlohmann/json.hpp`.

Download it:
```bash
mkdir -p vendor/nlohmann
curl -L https://github.com/nlohmann/json/releases/latest/download/json.hpp \
     -o vendor/nlohmann/json.hpp
```

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Usage

### 1. Register the runner

First, get a registration token from Gitea:
- Web UI: Repository → Settings → Actions → Runners → "New Runner"
- Or API: `POST /api/v1/runners/registration-token` with your admin token

Then register:

```bash
./act_runner register \
    --url https://gitea.example.com \
    --token PASTE_REGISTRATION_TOKEN_HERE \
    --name haiku-builder \
    --labels "haiku:host,haiku-latest:host"
```

### 2. Start the daemon

```bash
./act_runner daemon
```

Or with a custom config file and debug logging:

```bash
./act_runner daemon --config /boot/home/config/settings/act_runner/config.yaml \
                    --log-level debug
```

### 3. Run a workflow locally (no Gitea server needed)

The `run` subcommand executes a workflow file directly on the local machine
using the same engine as the daemon, but without connecting to any Gitea server.
Useful for testing workflows before pushing.

```bash
# Run all jobs in a workflow:
./act_runner run .gitea/workflows/ci.yml

# Simulate a specific event:
./act_runner run .gitea/workflows/ci.yml --event workflow_dispatch

# Run only one job (and its dependencies):
./act_runner run .gitea/workflows/ci.yml --job build

# Pass an event payload from a file:
./act_runner run .gitea/workflows/ci.yml --event push --payload @event.json

# Verbose output:
./act_runner run .gitea/workflows/ci.yml --log-level debug
```

The exit code mirrors the overall workflow result: `0` for success, `1` for failure.

### 4. Unregister the runner

Remove the local runner state (and optionally notify Gitea):

```bash
./act_runner unregister
```

This removes the local `.runner` state file and clears the token from the config.
If the runner still appears in Gitea's web UI, remove it manually:
`Site Administration → Actions → Runners`.

### 5. End-to-end smoke test (optional)

The `scripts/` directory contains a self-contained harness that stands up
a throwaway Gitea in Docker (on any Linux/macOS host), pushes a test
workflow, drives a real `act_runner daemon`, and asserts that the run
succeeds.  Sample workflows exercise the `CacheServer`, matrix expansion,
and `needs.<job>.outputs.*` wiring:

```bash
# On a Docker host:
./scripts/gitea_up.sh                 # produces scripts/.gitea-env.json

# Copy that JSON over, then on Haiku:
./scripts/e2e_smoke.sh                # runs hello.yml
./scripts/e2e_smoke.sh --all          # all four sample workflows
```

See `scripts/README.md` for the full workflow.

### 3. Write workflows targeting Haiku

```yaml
name: Build on Haiku
on: [push]

jobs:
  build:
    runs-on: haiku
    steps:
      - name: Check environment
        run: uname -a && echo "Hello from Haiku!"

      - name: Build project
        run: |
          cd $GITHUB_WORKSPACE
          make -j4
```

## Architecture

```
haiku-act-runner/
├── src/
│   ├── main.cpp                    # CLI: register | daemon
│   ├── config/
│   │   ├── Config.h/.cpp           # yaml-cpp config loading
│   │   └── RunnerState.h/.cpp      # JSON .runner token persistence
│   ├── client/
│   │   ├── GiteaClient.h/.cpp      # HTTP REST (libcurl)
│   │   └── RunnerClient.h/.cpp     # Connect-RPC (hand-coded proto + libcurl)
│   ├── runner/
│   │   ├── Poller.h/.cpp           # FetchTask loop + task dispatch
│   │   ├── TaskExecutor.h/.cpp     # Single job orchestration
│   │   ├── StepRunner.h/.cpp       # Step execution (posix_spawn)
│   │   └── LogForwarder.h/.cpp     # Batched UpdateLog streaming
│   ├── process/
│   │   ├── ProcessSpawner.h/.cpp   # posix_spawn wrapper
│   │   └── EnvManager.h/.cpp       # GITHUB_OUTPUT/ENV/PATH protocol
│   └── workflow/
│       ├── WorkflowParser.h/.cpp   # YAML workflow parsing (yaml-cpp)
│       ├── ExprEvaluator.h/.cpp    # ${{ }} expression engine
│       ├── ContextBuilder.h/.cpp   # github/env/runner/steps contexts
│       └── MatrixExpander.h/.cpp   # Matrix cross-product expansion
├── proto/
│   └── runner.proto                # Service definition (reference)
├── vendor/
│   └── nlohmann/json.hpp           # bundled JSON library
├── CMakeLists.txt
└── config.yaml.example
```

## Key Design Decisions

1. **Host executor only** — No Docker on Haiku; steps run directly on host via `posix_spawn()`.
2. **Connect-RPC over libcurl** — Avoids `gRPC C++` runtime dependency; uses HTTP/1.1 + hand-coded protobuf.
3. **`posix_spawn()` over `fork()`** — Safer in multi-threaded Haiku process.
4. **`poll()` (implicit via libcurl)** — No `epoll`/`kqueue` (Linux/BSD only).
5. **`find_directory()` for paths** — Never hardcodes `/etc/` or `/usr/`.
6. **`sigaction()` for shutdown** — Avoids Linux-only `signalfd`.
7. **`std::counting_semaphore`** (C++20) — Capacity limiting for concurrent jobs.

## Haiku-specific notes

- Settings path: `B_USER_SETTINGS_DIRECTORY/act_runner/` → `/boot/home/config/settings/act_runner/`
- Temp workspaces: `/tmp/act_runner_<task_id>_<random>/`
- No `epoll`, `inotify`, `/proc`, or `signalfd` — all avoided.
- Tested with GCC 13/14 (`-std=c++20`) on x86_64 Haiku.
