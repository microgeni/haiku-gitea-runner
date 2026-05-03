# haiku-act-runner

A native C++20 port of the [Gitea Actions Runner](https://gitea.com/gitea/act_runner)
for **Haiku OS** — no Docker, no Go toolchain, no gRPC runtime required.

---

## Features

| Feature | Status |
|---------|--------|
| Registration via HTTP REST | ✅ |
| Connect-RPC transport (libcurl + hand-coded protobuf) | ✅ |
| FetchTask long-poll loop with capacity semaphore | ✅ |
| `run:` step execution via `load_image()` / `posix_spawn()` | ✅ |
| Log streaming (`UpdateLog` batched RPC) | ✅ |
| `$GITHUB_OUTPUT` / `$GITHUB_ENV` / `$GITHUB_PATH` protocol | ✅ |
| `${{ }}` expression evaluator (full GitHub Actions spec) | ✅ |
| `github.*`, `env.*`, `runner.*`, `steps.*`, `needs.*` contexts | ✅ |
| Job matrix expansion (include/exclude) | ✅ |
| `needs:` dependency graph (topological sort + wave schedule) | ✅ |
| `uses:` composite and JavaScript actions | ✅ |
| `actions/checkout` (real git clone) | ✅ |
| `actions/cache` / artifact APIs (local HTTP server) | ✅ |
| `if:` conditions on steps and jobs | ✅ |
| Job-level and step-level timeouts | ✅ |
| `continue-on-error:` / `fail-fast:` / `max-parallel:` | ✅ |
| `on:` trigger parsing (push/pull_request/schedule/…) | ✅ |
| Watchdog process (auto-restart on crash) | ✅ |
| Haiku `launch_daemon` service descriptor | ✅ |
| Local `run` subcommand (no Gitea server needed) | ✅ |
| Graceful shutdown on SIGINT/SIGTERM | ✅ |
| Secret masking in logs | ✅ |
| `hashFiles()` with recursive `**` glob (SHA-256) | ✅ |

---

## Requirements

### Build host: Haiku OS (64-bit, R1/β5 or later)

```bash
pkgman install curl_devel yaml_cpp_devel protobuf_devel libmicrohttpd_devel openssl_devel cmake
```

The [`nlohmann/json`](https://github.com/nlohmann/json) header is bundled in
`vendor/nlohmann/json.hpp`. If you cloned without it, fetch it once:

```bash
mkdir -p vendor/nlohmann
curl -L https://github.com/nlohmann/json/releases/latest/download/json.hpp \
     -o vendor/nlohmann/json.hpp
```

### Runtime dependencies (Haiku)

| Package | Purpose |
|---------|---------|
| `curl` | HTTP transport for RPC and registration |
| `openssl` | TLS and SHA-256 (used by `hashFiles()`) |
| `libmicrohttpd` | Local cache/artifact HTTP server |
| `git` *(optional)* | `actions/checkout` and remote action fetching |
| `nodejs20` *(optional)* | JavaScript actions (`actions/cache`, etc.) |

---

## Build

```bash
git clone https://example.com/haiku-act-runner.git
cd haiku-act-runner

# Bundle nlohmann/json if not already present
mkdir -p vendor/nlohmann
curl -sSL https://github.com/nlohmann/json/releases/latest/download/json.hpp \
     -o vendor/nlohmann/json.hpp

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

The resulting binary is `build/act_runner`.

### Build options

| CMake variable | Default | Description |
|----------------|---------|-------------|
| `CMAKE_BUILD_TYPE` | `Release` | `Release` / `Debug` / `RelWithDebInfo` |
| `RUNNER_VERSION` | `0.2.0` | Version string embedded in `--version` output |

---

## Quick start

### Step 1 — Get a registration token from Gitea

There are three runner scopes. **Global (instance-level) is recommended** — it
accepts jobs from any repository on the Gitea instance without needing a
separate registration per repo.

| Scope | Token location | Receives jobs from |
|-------|---------------|-------------------|
| **Global** ✅ | Site Administration → Actions → Runners → *Create runner* | All repositories on the instance |
| Organisation | Organisation Settings → Actions → Runners | Repos in that org only |
| Repository | Repo Settings → Actions → Runners | That repo only |

> **Note:** Individual (repo/org) runners will not pick up jobs from other
> repositories. If the runner appears idle despite queued jobs, check that it
> is registered at the correct scope. Registering as **Global** is the
> simplest setup and works immediately.

### Step 2 — Register the runner

```bash
./act_runner register \
    --url https://gitea.example.com \
    --token PASTE_TOKEN_HERE \
    --name haiku-builder \
    --labels "haiku:host,haiku-latest:host,haiku-x64:host"
```

The label format is `<label-name>:<executor-type>`. The `:host` suffix tells
Gitea this is a host-executor runner (no Docker). The part before `:` is what
workflows use in `runs-on:`.

This writes `/boot/home/config/settings/act_runner/config.yaml` and
`/boot/home/config/settings/act_runner/.runner`.

To get a global registration token via the API (requires admin API key):

```bash
curl -s -X POST https://gitea.example.com/api/v1/admin/runners/registration-token \
     -H "Authorization: token <admin-api-key>"
```

### Step 3 — Start the daemon

```bash
./act_runner daemon
```

The daemon long-polls Gitea for jobs, executes them (host-executor only — no Docker),
and streams logs back in real time.  Press `Ctrl-C` or send `SIGTERM` to stop
gracefully.

---

## Subcommands

```
act_runner register   --url <url> --token <token> [--name <name>] [--labels <l,...>]
                      [--config <path>] [--insecure]
act_runner daemon     [--config <path>] [--log-level debug|info|warn|error]
act_runner run        <workflow.yml> [--event <name>] [--payload <json|@file>]
                      [--job <id>] [--log-level <level>] [--retry <n>]
act_runner unregister [--config <path>]
act_runner version
act_runner help
```

### `daemon`

Starts the runner and polls Gitea for jobs indefinitely.  On Haiku, it
automatically installs a watchdog process that restarts the daemon on crash
with exponential back-off (max 5 minutes between retries).

### `run` — local workflow execution (no server required)

Runs a workflow YAML file directly on the local machine without connecting to
any Gitea server.  Useful for testing workflows before pushing.

```bash
# Run all jobs:
./act_runner run .gitea/workflows/ci.yml

# Simulate a different event:
./act_runner run .gitea/workflows/ci.yml --event pull_request

# Run only one job and its dependencies:
./act_runner run .gitea/workflows/ci.yml --job build

# Pass a JSON event payload:
./act_runner run .gitea/workflows/ci.yml --event push --payload '{"ref":"refs/heads/main"}'

# Read payload from a file:
./act_runner run .gitea/workflows/ci.yml --event push --payload @push_event.json

# Auto-retry on Haiku SIGKILLTHR transients:
./act_runner run .gitea/workflows/ci.yml --retry 3

# Verbose:
./act_runner run .gitea/workflows/ci.yml --log-level debug
```

Exit code: `0` = all jobs succeeded, `1` = one or more jobs failed.

> **Tip (Haiku):** On Haiku, `posix_spawn()` can occasionally deliver
> `SIGKILLTHR` (signal 7) to a newly-started child when many threads are
> running simultaneously.  `--retry 3` re-runs the entire workflow up to 3
> additional times to recover from these transients.

### `unregister`

Removes the local runner state (`~/.runner` equivalent) and clears the token
from `config.yaml`.  Does not require network access.  If the runner still
appears in Gitea's web UI, delete it there manually.

```bash
./act_runner unregister
# or with a custom config:
./act_runner unregister --config /path/to/config.yaml
```

---

## Configuration

The config file is created automatically by `register`.  Default location:

```
/boot/home/config/settings/act_runner/config.yaml
```

Full reference:

```yaml
gitea_url: https://gitea.example.com   # required
name: taurus                            # display name (default: hostname)
capacity: 1                             # max concurrent jobs  (1 – 16)
fetch_timeout: 30                       # FetchTask long-poll timeout, seconds
fetch_interval: 2                       # delay between FetchTask calls, seconds
insecure: false                         # skip TLS certificate verification

log_level: info                         # debug | info | warn | error

labels:
  - haiku:host                          # custom label — "host" executor on Haiku
  - haiku
  - haiku-x64

# actions/cache and artifact API server
cache:
  enabled: true          # start the built-in cache server
  host: 127.0.0.1        # bind address (loopback recommended)
  port: 0                # 0 = pick an ephemeral port
  max_age_days: 7        # purge entries older than N days on daemon startup
```

---

## Workflow YAML support

### Triggers (`on:`)

All three `on:` forms are parsed and used to validate `act_runner run --event`:

```yaml
on: push                              # scalar
on: [push, pull_request]              # sequence
on:                                   # map with filters
  push:
    branches: [main, develop]
    paths: ['src/**', 'CMakeLists.txt']
  pull_request:
    types: [opened, synchronize]
  release:
    tags: ['v*']
```

When you run `act_runner run workflow.yml --event release` and the workflow's
`on:` block doesn't include `release`, a warning is printed but execution
proceeds (useful for manual testing).

### Expression language

Full GitHub Actions expression syntax is supported:

```yaml
# Operators: ==  !=  <  <=  >  >=  &&  ||  !
if: github.event_name == 'push' && startsWith(github.ref, 'refs/heads/')

# Functions
if: contains(github.ref, 'feature')
if: startsWith(github.ref, 'refs/tags/v')
if: endsWith(github.ref, '-stable')
run: echo "hash=${{ hashFiles('**/package-lock.json') }}"
run: echo "json=${{ toJSON(github.event) }}"

# Status functions (for post-failure steps)
if: always()
if: failure()
if: success()
if: cancelled()

# Context access
run: echo "SHA=${{ github.sha }}"
run: echo "VAR=${{ env.MY_VAR }}"
run: echo "Output=${{ steps.build.outputs.artifact }}"
run: echo "Dep output=${{ needs.build.outputs.version }}"
```

Null coercion follows the GitHub Actions specification:
`null == false` → `true`, `null == 0` → `true`, `null == ''` → `true`.

### Job features

```yaml
jobs:
  build:
    runs-on: haiku
    timeout-minutes: 60          # job-level timeout
    continue-on-error: false     # job-level continue-on-error
    strategy:
      fail-fast: false           # don't abort sibling jobs on failure
      max-parallel: 2            # max concurrent jobs within this wave
      matrix:
        config: [debug, release]
        arch: [x64]
    env:
      BUILD_TYPE: ${{ matrix.config }}
    outputs:
      artifact: ${{ steps.build.outputs.artifact }}
    steps:
      - id: build
        name: Build
        run: make -j4 CONFIG=${{ matrix.config }}
        timeout-minutes: 30
        continue-on-error: false
        if: success()
        env:
          CC: gcc
```

### Actions (`uses:`)

```yaml
steps:
  # Remote action (fetched from GitHub or configured Gitea instance)
  - uses: actions/checkout@v4

  # With inputs
  - uses: actions/cache@v4
    with:
      path: ~/.cache
      key: ${{ runner.os }}-${{ hashFiles('**/package-lock.json') }}

  # Local composite action in the repo
  - uses: ./.gitea/actions/my-action

  # Composite actions support recursion (up to depth 10)
```

> **Docker actions** (`docker://image`) are logged as an error and skipped.
> Haiku has no Docker runtime.

---

## Writing workflows for Haiku

```yaml
name: CI on Haiku

on:
  push:
    branches: [main]
  pull_request:

jobs:
  build:
    runs-on: haiku          # or whatever label you registered with
    timeout-minutes: 30
    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Install deps
        run: pkgman install -y cmake yaml_cpp_devel curl_devel

      - name: Build
        run: |
          mkdir -p build
          cmake -B build -DCMAKE_BUILD_TYPE=Release
          cmake --build build -- -j$(nproc)
        env:
          HAIKU_VERSION: R1/beta5

      - name: Test
        run: cmake --build build --target test
        continue-on-error: false
```

---

## Running tests

```bash
cd build
ctest --output-on-failure      # run all 14 test suites
ctest -R test_expr_evaluator   # run a single suite
```

Test suites:

| Suite | What it tests |
|-------|--------------|
| `test_expr_evaluator` | 71 expression evaluator unit tests |
| `test_workflow_parser` | 39 YAML parser tests incl. `on:` triggers |
| `test_matrix_expander` | Matrix cross-product expansion |
| `test_env_manager` | `$GITHUB_OUTPUT` / `$GITHUB_ENV` protocol |
| `test_process_spawner` | Process spawning, pipes, timeouts |
| `test_job_graph` | Topological sort, cycle detection, wave schedule |
| `test_context_builder` | Expression context population |
| `test_runner_client` | Connect-RPC protobuf encode/decode |
| `test_action_runner` | Local and remote `uses:` action resolution |
| `test_task_executor` | End-to-end job execution with mock client |
| `test_cache_server` | Cache and artifact HTTP API (libmicrohttpd) |
| `test_workflow_orchestrator` | Multi-job wave dispatch + `cancel()` |
| `test_poller` | FetchTask loop, capacity semaphore, shutdown |
| `test_main_cli` | Black-box CLI integration tests |

---

## Architecture

```
haiku-act-runner/
├── src/
│   ├── main.cpp                        # CLI, signal handling, watchdog
│   ├── config/
│   │   ├── Config.h/.cpp               # YAML config (yaml-cpp)
│   │   └── RunnerState.h/.cpp          # .runner JSON persistence
│   ├── client/
│   │   ├── GiteaClient.h/.cpp          # HTTP REST (libcurl)
│   │   ├── RunnerClient.h/.cpp         # Connect-RPC + protobuf (libcurl)
│   │   ├── IRunnerClient.h             # Abstract interface (testable)
│   │   ├── LocalRunnerClient.h         # No-op impl for `run` subcommand
│   │   └── RunnerDtos.h                # Data transfer objects
│   ├── runner/
│   │   ├── Poller.h/.cpp               # FetchTask loop + thread pool
│   │   ├── TaskExecutor.h/.cpp         # Single-job orchestration
│   │   ├── StepRunner.h/.cpp           # Step execution
│   │   ├── LogForwarder.h/.cpp         # Batched log streaming
│   │   ├── JobGraph.h/.cpp             # Dependency graph / wave schedule
│   │   └── WorkflowOrchestrator.h/.cpp # Local multi-job execution
│   ├── process/
│   │   ├── ProcessSpawner.h/.cpp       # load_image/posix_spawn + pipes
│   │   └── EnvManager.h/.cpp           # Protocol file management
│   ├── workflow/
│   │   ├── WorkflowParser.h/.cpp       # YAML → Workflow struct (yaml-cpp)
│   │   ├── ExprEvaluator.h/.cpp        # ${{ }} recursive-descent parser
│   │   ├── ContextBuilder.h/.cpp       # Context population from TaskDto
│   │   └── MatrixExpander.h/.cpp       # Cross-product + include/exclude
│   ├── action/
│   │   └── ActionRunner.h/.cpp         # composite/JS/checkout action support
│   ├── cache/
│   │   └── CacheServer.h/.cpp          # cache + artifact HTTP server
│   └── util/
│       └── Logger.h/.cpp               # Thread-safe levelled logger
├── proto/
│   └── runner.proto                    # Gitea RunnerService definition (ref)
├── tests/                              # 14 test suites
├── vendor/nlohmann/json.hpp            # Bundled JSON library
├── haiku-packaging/
│   ├── act_runner.recipe               # HaikuPorts recipe (local reference)
│   └── launch_daemon/                  # launch_daemon service descriptor
├── scripts/
│   ├── gitea_up.sh                     # Spin up throwaway Gitea in Docker
│   ├── e2e_smoke.sh                    # End-to-end integration test
│   └── workflows/                      # Sample workflows for smoke tests
├── CMakeLists.txt
└── config.yaml.example
```

---

## Haiku-specific design notes

### Why `load_image()` instead of `posix_spawn()`

Haiku's `posix_spawn()` is implemented via an intermediate spawner thread
created inside the parent team.  When that thread exits after a successful
`load_image()`, the kernel's thread-exit path can incorrectly deliver
`SIGKILLTHR` (signal 21, un-catchable) to the parent, killing the daemon.

We use `load_image()` directly — a single kernel syscall that creates the
child team without any intermediate thread.  A global mutex
(`s_load_image_mutex`) serialises concurrent spawns to avoid a separate
race condition present when multiple threads call `load_image()` simultaneously.

### Signal handling

| Signal | Handler |
|--------|---------|
| `SIGINT`, `SIGTERM` | Set `g_shutdown` flag → graceful stop |
| `SIGCHLD` | `SIG_IGN` (child cleanup via `waitpid`) |
| `SIGSEGV`, `SIGBUS` | `siglongjmp` recovery inside worker (logs error, marks task failed) |
| `SIGTRAP`, `SIGUSR2` | Benign handler (Haiku `load_image()` internals) |

`signalfd`, `eventfd`, and `timerfd` are Linux-only and are not used.

### Paths

All paths use `find_directory()` (Haiku API) rather than hardcoded prefixes:

| Purpose | Path |
|---------|------|
| Config + state | `B_USER_SETTINGS_DIRECTORY/act_runner/` |
| Job workspaces | `B_SYSTEM_TEMP_DIRECTORY/act_runner_<id>_<hex>/` |
| Action cache | `B_USER_SETTINGS_DIRECTORY/act_runner/action_cache/` |

---

## Known limitations

| Limitation | Workaround |
|-----------|-----------|
| Docker actions (`docker://…`) | Not supported — Haiku has no Docker |
| `actions/cache` without Node.js | Emits warning, treated as cache miss |
| JS actions without Node.js | Emits error, step returns failure (or continue-on-error) |
| `join()` with array values | Stringifies first arg only (GHA arrays not fully modelled) |
| Reusable workflows (`workflow_call`) | Not yet implemented |
| OIDC / GitHub-App tokens | Not applicable (Gitea-only runner) |

---

## License

MIT — see [LICENSE](LICENSE).

The original [act_runner](https://gitea.com/gitea/act_runner) is also MIT-licensed.
