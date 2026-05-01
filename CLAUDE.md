# Gitea Runner → C++ Port for Haiku OS

## Project Goal

Investigate and implement a port of the **Gitea Actions Runner** (`act_runner`) to **Haiku OS** in C++. The original runner is written in Go; this project re-implements it natively for Haiku using C++17/20.

---

## 1. Original Project Reference

| Item | Detail |
|------|--------|
| **Source repo** | https://gitea.com/gitea/act_runner |
| **Language** | Go 1.21+ |
| **License** | MIT |
| **Protocol** | gRPC (Connect-RPC / HTTP/2) + REST for registration |
| **Protobuf defs** | https://code.gitea.io/actions-proto-go |
| **Execution engine** | https://github.com/nektos/act |

---

## 2. How the Original Runner Works

### 2.1 Registration (one-time, HTTP REST)

```
POST /api/v1/runners/registration-token   →  exchange token
gRPC Register(reg_token)                  →  runner_token + UUID
```

State stored in `.runner` (JSON: token, UUID, name, labels).

### 2.2 Runtime Loop (gRPC / Connect-RPC over HTTP/2)

```
loop:
  gRPC FetchTask(labels, capacity)    →  task payload (long-poll)
  spawn goroutine:
    gRPC UpdateTask(running)
    execute job (via nektos/act)
    gRPC UpdateLog(batched lines)     →  repeated
    gRPC UpdateLog(no_more=true)
    gRPC UpdateTask(success|failure)
  gRPC Ping()                         →  periodic keepalive
```

### 2.3 gRPC Service API (from `actions-proto-go`)

```protobuf
service Runner {
  rpc Ping(PingRequest)             returns (PingResponse);
  rpc Register(RegisterRequest)     returns (RegisterResponse);
  rpc FetchTask(FetchTaskRequest)   returns (FetchTaskResponse);
  rpc UpdateTask(UpdateTaskRequest) returns (UpdateTaskResponse);
  rpc UpdateLog(UpdateLogRequest)   returns (UpdateLogResponse);
}
```

Authentication: runner token in gRPC metadata header `x-runner-token`.

### 2.4 Execution Modes

| Mode | Description | Haiku feasibility |
|------|-------------|-------------------|
| Docker executor | Runs each job inside a Linux container | ❌ No Docker on Haiku |
| Host executor | Runs steps directly via shell on the host | ✅ Target mode for Haiku |

### 2.5 Configuration (YAML)

```yaml
gitea_url: https://gitea.example.com
runner_token: ""          # filled after registration
capacity: 4               # max concurrent jobs
labels:
  - haiku:host            # custom label for Haiku host runner
```

---

## 3. Haiku OS Development Environment

### 3.1 Toolchain

| Tool | Status |
|------|--------|
| GCC 13/14 (`x86_64`) | ✅ Default on 64-bit Haiku |
| Clang | ✅ Available via `pkgman install llvm17_clang` |
| CMake | ✅ `pkgman install cmake` |
| Make / Meson | ✅ Available |
| `pkg-config` | ✅ Present; libs in `/system/develop/lib/pkgconfig/` |

Use `-std=c++17` or `-std=c++20`. C++20 is well-supported on GCC 13.

### 3.2 Package Manager

```bash
pkgman install <package>          # install
pkgman install <package>_devel    # install with headers + link libs
pkgman search <name>              # search
HaikuDepot                        # GUI browser
```

Packages are mounted as virtual read-only filesystems (packagefs). **Never hardcode `/system/`** paths; use `find_directory()`.

Key paths:

```
/system/develop/headers/   # headers (like /usr/include)
/system/develop/lib/       # link-time libs
/system/lib/               # runtime libs
/boot/home/config/         # user-installed packages
/tmp                       # writable temp
```

### 3.3 POSIX Compatibility

| Feature | Status | Notes |
|---------|--------|-------|
| pthreads | ✅ Full | `std::thread` works |
| BSD sockets | ✅ Full | IPv4 + IPv6 |
| `poll()` / `select()` | ✅ | Use instead of epoll |
| `fork()` / `exec()` | ✅ | Works; prefer `posix_spawn()` |
| `posix_spawn()` | ✅ Preferred | Better than fork() |
| signals | ✅ Full | `sigaction()`, `sigwaitinfo()` |
| `mmap()` | ✅ | |
| Unix domain sockets | ✅ | |
| **`epoll`** | ❌ | Linux-only; use `poll()` |
| **`kqueue`** | ❌ | BSD-only |
| **`signalfd`** | ❌ | Linux-only; use `sigwaitinfo()` |
| **`eventfd`** | ❌ | Linux-only; use pipes |
| **`timerfd`** | ❌ | Linux-only; use `timer_create()` |
| **`inotify`** | ❌ | Linux-only; use `watch_node()` |
| **`/proc`** | ❌ | Use Haiku `<OS.h>` APIs |
| **`/sys`** | ❌ | Not present |

---

## 4. C++ Architecture for the Port

### 4.1 Component Map

```
haiku-act-runner/
├── src/
│   ├── main.cpp                  # entry point, CLI parsing
│   ├── config/
│   │   ├── Config.h/.cpp         # YAML config loading (yaml-cpp)
│   │   └── RunnerState.h/.cpp    # .runner token/UUID persistence (JSON)
│   ├── client/
│   │   ├── GiteaClient.h/.cpp    # HTTP REST (registration) via libcurl
│   │   └── RunnerClient.h/.cpp   # gRPC client (grpc++)
│   ├── runner/
│   │   ├── Poller.h/.cpp         # FetchTask loop, semaphore, thread pool
│   │   ├── TaskExecutor.h/.cpp   # orchestrates a single job
│   │   ├── StepRunner.h/.cpp     # runs one step (shell command)
│   │   ├── LogForwarder.h/.cpp   # batches + streams log lines via gRPC
│   │   └── ActionFetcher.h/.cpp  # downloads remote actions (git/zip)
│   ├── workflow/
│   │   ├── WorkflowParser.h/.cpp # parse workflow YAML (yaml-cpp)
│   │   ├── ExprEvaluator.h/.cpp  # ${{ }} expression evaluator
│   │   ├── ContextBuilder.h/.cpp # github/env/runner context objects
│   │   └── MatrixExpander.h/.cpp # job matrix expansion
│   ├── process/
│   │   ├── ProcessSpawner.h/.cpp # posix_spawn / waitpid wrapper
│   │   └── EnvManager.h/.cpp     # env vars, $GITHUB_OUTPUT etc.
│   └── cache/
│       └── CacheServer.h/.cpp    # optional HTTP cache API server
├── proto/                        # generated from actions-proto-go .proto files
│   ├── runner.pb.h/.cc
│   └── runner.grpc.pb.h/.cc
├── CMakeLists.txt
└── config.yaml.example
```

### 4.2 Threading Model

Use `std::thread` + `std::counting_semaphore` (C++20):

```
Main thread:        CLI → config load → registration → start Poller
Poller thread:      FetchTask loop → dispatch TaskExecutor threads
TaskExecutor[N]:    parse task → run steps sequentially → report result
LogForwarder thread: drain queue → batch UpdateLog RPCs
```

No epoll needed — gRPC C++ uses its own polling abstraction internally.

---

## 5. Key Dependencies

### 5.1 Library Availability on Haiku

| Library | Purpose | Install | Notes |
|---------|---------|---------|-------|
| `grpc++` + `protobuf` | gRPC client + protobuf | `pkgman install grpc_devel protobuf_devel` | Check HaikuPorts; may need manual build |
| `libcurl` | HTTP REST (registration, artifact upload) | `pkgman install curl_devel` | ✅ Well-supported |
| `yaml-cpp` | Config + workflow YAML parsing | `pkgman install yaml_cpp_devel` | Check availability |
| `nlohmann/json` | JSON (runner state, REST payloads) | Header-only; bundle in repo | ✅ Easy |
| `libsqlite3` | Local state DB (optional) | `pkgman install sqlite_devel` | ✅ Works |
| `openssl` | TLS for HTTP/gRPC | `pkgman install openssl_devel` | ✅ |
| `libgit2` | Fetching remote actions | `pkgman install libgit2_devel` | Check availability |
| `libarchive` | Extracting action zip/tar | `pkgman install libarchive_devel` | Check availability |
| `boost` (optional) | Boost.Asio for async I/O | `pkgman install boost1_83_devel` | Uses `select()` on Haiku |

### 5.2 gRPC on Haiku

gRPC C++ is the most critical and risky dependency:

- gRPC C++ core is POSIX-based and should compile on Haiku.
- It bundles `absl`, `boringssl`, `re2`, `c-ares` — all potentially buildable.
- The polling engine on non-Linux/BSD platforms falls back to `poll()`.
- **Check HaikuPorts** first: `https://github.com/haikuports/haikuports`
- If not available, build from source with polling backend `GRPC_POSIX_NO_SPECIAL_WAKEUP_FD`.

Alternatively, implement the **Connect-RPC protocol manually** over HTTP/2 using `libcurl` (which supports HTTP/2 via nghttp2) — Gitea accepts Connect-RPC framing over HTTP/1.1 too.

### 5.3 Protobuf Generation

```bash
protoc --cpp_out=proto/ --grpc_out=proto/ \
       --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
       runner.proto
```

Get the `.proto` file from: https://code.gitea.io/actions-proto-go

---

## 6. Porting Challenges & Mitigations

### 6.1 Workflow/Expression Engine (Hardest)

The Go port uses `nektos/act` — a full GitHub Actions execution engine. In C++ this must be re-implemented:

| Sub-feature | Complexity | Approach |
|-------------|-----------|---------|
| YAML workflow parser | Medium | `yaml-cpp` |
| `${{ }}` expression evaluator | High | Custom recursive-descent parser |
| Matrix expansion | Medium | Nested loop over matrix keys |
| `if:` condition evaluation | Medium | Reuse expression evaluator |
| `$GITHUB_OUTPUT` / `$GITHUB_ENV` protocol | Low | Parse temp files after each step |
| Composite action recursion | High | Recursive step execution |
| Remote action fetching | Medium | `libgit2` or shell-out to `git` |

**GitHub Actions expression language spec**: https://docs.github.com/en/actions/learn-github-actions/expressions

Start with a minimal subset: literal strings/numbers, `env.*`, `github.*`, `==`, `!=`, `&&`, `||`, `!`.

### 6.2 No Docker → Host Executor Only

On Haiku, jobs run directly on the host (no container isolation):

- Each job gets a temp workspace: `find_directory(B_TEMP_DIRECTORY)` + UUID subdirectory.
- Steps run via `posix_spawn()` with inherited environment + job-specific variables.
- Shell: `/bin/sh` (available on Haiku) or `bash` if installed.
- Cleanup: remove workspace directory after job completion.

### 6.3 Process Spawning

Replace Go's `os/exec` with `posix_spawn`:

```cpp
#include <spawn.h>
#include <sys/wait.h>

posix_spawn_file_actions_t actions;
posix_spawnattr_t attr;
// set up stdin/stdout/stderr redirects
pid_t pid;
posix_spawn(&pid, "/bin/sh", &actions, &attr, argv, envp);
int status;
waitpid(pid, &status, 0);
```

### 6.4 Log Streaming

Implement a producer-consumer queue:

```cpp
std::queue<LogLine> queue_;
std::mutex mutex_;
std::condition_variable cv_;
```

Background thread drains the queue and calls `UpdateLog` gRPC RPC in batches (e.g. every 1 second or 50 lines).

### 6.5 Graceful Shutdown

On Haiku, `SIGINT` and `SIGTERM` work normally:

```cpp
struct sigaction sa{};
sa.sa_handler = [](int) { g_shutdown.store(true); };
sigaction(SIGINT,  &sa, nullptr);
sigaction(SIGTERM, &sa, nullptr);
```

Avoid `signalfd` (Linux-only). Use `sa_handler` or `sigwaitinfo()` in a dedicated thread.

### 6.6 Paths

Always use `find_directory()`:

```cpp
#include <FindDirectory.h>
#include <Path.h>

BPath settingsPath;
find_directory(B_USER_SETTINGS_DIRECTORY, &settingsPath);
settingsPath.Append("act_runner");
// → /boot/home/config/settings/act_runner/
```

### 6.7 gRPC Build Workaround

If gRPC C++ is unavailable as a package, the minimum viable alternative is to implement the **Connect-RPC protocol over HTTP/1.1** using libcurl:

- Connect-RPC encodes protobuf in the HTTP body with `Content-Type: application/proto`.
- Each RPC is a `POST` to `/gitea.actions.v1.RunnerService/<MethodName>`.
- Request body: 5-byte envelope (1 flag byte + 4-byte big-endian length) + serialized protobuf.
- libcurl handles TLS, HTTP/1.1, auth headers, and streaming response bodies.

This avoids the gRPC C++ dependency entirely.

---

## 7. Build System

### 7.1 CMakeLists.txt Structure

```cmake
cmake_minimum_required(VERSION 3.20)
project(haiku-act-runner CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)     # or use pkg-config fallback
find_package(yaml-cpp REQUIRED)

add_executable(act_runner
    src/main.cpp
    src/config/Config.cpp
    src/client/GiteaClient.cpp
    src/client/RunnerClient.cpp
    src/runner/Poller.cpp
    src/runner/TaskExecutor.cpp
    src/runner/StepRunner.cpp
    src/runner/LogForwarder.cpp
    src/workflow/WorkflowParser.cpp
    src/workflow/ExprEvaluator.cpp
    proto/runner.pb.cc
    proto/runner.grpc.pb.cc
)

target_link_libraries(act_runner
    CURL::libcurl
    protobuf::libprotobuf
    gRPC::grpc++
    yaml-cpp
    pthread
)

if(HAIKU)
    target_link_libraries(act_runner network be)
endif()
```

### 7.2 Build Commands

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

---

## 8. Implementation Roadmap

### Phase 1 – Skeleton & Registration
- [ ] Project structure + CMakeLists.txt
- [ ] YAML config loading (`yaml-cpp`)
- [ ] HTTP REST registration with Gitea (libcurl)
- [ ] Persist runner token to `.runner` JSON file

### Phase 2 – gRPC Client
- [ ] Build / install gRPC C++ on Haiku (or implement Connect-RPC fallback)
- [ ] Generate protobuf stubs from `actions-proto-go`
- [ ] Implement `Ping`, `FetchTask`, `UpdateTask`, `UpdateLog`
- [ ] Auth header interceptor

### Phase 3 – Job Execution (minimal)
- [ ] Workflow YAML parser (top-level structure only)
- [ ] `run:` step executor via `posix_spawn()`
- [ ] Environment variable pass-through
- [ ] `$GITHUB_OUTPUT` / `$GITHUB_ENV` file protocol
- [ ] Log capture → LogForwarder

### Phase 4 – Expression Engine
- [ ] Tokenizer and recursive-descent parser for `${{ }}`
- [ ] Built-in functions: `contains()`, `startsWith()`, `format()`, `toJSON()`, etc.
- [ ] Context objects: `github.*`, `env.*`, `runner.*`, `steps.*`

### Phase 5 – Full Workflow Features
- [ ] Job matrix expansion
- [ ] `if:` conditions on steps and jobs
- [ ] `needs:` dependencies between jobs
- [ ] `uses:` — local and remote action references
- [ ] Composite actions
- [ ] Remote action fetching via libgit2

### Phase 6 – Haiku Polish
- [ ] Haiku launch_daemon integration (service descriptor)
- [ ] Use `find_directory()` for all paths
- [ ] HPKG packaging recipe for HaikuPorts

> **Note:** We will **not** submit a PR to HaikuPorts. The recipe in
> `haiku-packaging/act_runner.recipe` is provided for reference and local
> packaging only.

---

## 9. Useful References

| Resource | URL |
|----------|-----|
| act_runner source | https://gitea.com/gitea/act_runner |
| actions-proto-go | https://code.gitea.io/actions-proto-go |
| nektos/act (reference impl) | https://github.com/nektos/act |
| GitHub Actions expression spec | https://docs.github.com/en/actions/learn-github-actions/expressions |
| GitHub Actions workflow syntax | https://docs.github.com/en/actions/using-workflows/workflow-syntax-for-github-actions |
| Connect-RPC protocol spec | https://connectrpc.com/docs/protocol |
| Haiku Book (API reference) | https://api.haiku-os.org |
| HaikuPorts | https://github.com/haikuports/haikuports |
| Haiku Depot | https://depot.haiku-os.org |
| gRPC C++ docs | https://grpc.io/docs/languages/cpp/ |
| yaml-cpp | https://github.com/jbeder/yaml-cpp |
| nlohmann/json | https://github.com/nlohmann/json |
| libgit2 | https://libgit2.org |

---

## 10. Key Design Decisions

1. **Host executor only** — no Docker on Haiku; run steps directly in process.
2. **`posix_spawn()` over `fork()`** — safer in multithreaded Haiku process.
3. **`poll()` over epoll/kqueue** — the only portable event mechanism on Haiku.
4. **Connect-RPC over HTTP/1.1 as gRPC fallback** — if gRPC C++ doesn't build, use libcurl + manual protobuf framing.
5. **`find_directory()` for all paths** — never hardcode `/system/`, `/etc/`, `/usr/`.
6. **`std::counting_semaphore` for capacity** — limit concurrent jobs cleanly (C++20).
7. **Minimal expression engine first** — implement the 20% of expression features that cover 80% of real-world workflows.
8. **`sigaction()` for shutdown** — avoid Linux-only `signalfd`.
