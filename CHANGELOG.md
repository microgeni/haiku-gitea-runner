# Changelog

## [0.3.0] - 2026-05-03

### Fixes
- **Daemon log missing after reboot** — `act_runner-launch.sh` now redirects
  stdout/stderr to `daemon.log`; previously output was swallowed by
  `launch_daemon`.
- **`install-service.sh` variable order bug** — `$SETTINGS_DIR` was referenced
  in the `run.sh` write step before being defined; moved `mkdir -p` to where
  it is first needed.

### Improvements
- **`work_dir` fully configurable** — previously `work_dir` was read from
  `config.yaml` and used for job workspace creation, but three subsystems
  still hardcoded `/tmp`:
  - `StepRunner`: `ActionCache` now uses `<work_dir>/act_runner_actions`
  - `ContextBuilder`: `runner.temp` → `<work_dir>/tmp`,
    `runner.tool_cache` → `<work_dir>/tool_cache`
  - `TaskExecutor`: passes `work_dir` to both subsystems
- **`config.yaml.example`**: documented `work_dir` with explanation of all
  paths it controls (workspaces, action cache, `runner.temp`,
  `runner.tool_cache`).

### Packaging / CI
- `run.sh` kept in sync by `install-service.sh` on every install/upgrade.
- `launch_roster restart` on deploy instead of daemon kill.
- `uuid` embedded in `config.yaml` after registration (persists across
  reinstalls without re-registration).
- `launch_daemon` boot service descriptor bundled and auto-installed.

## [0.2.0] - 2026-05-03

### First public release

- Native C++20 port of the Gitea Actions runner for Haiku OS
- Connect-RPC transport over libcurl (no gRPC runtime required)
- Host executor: steps run directly via `load_image()` / `posix_spawn()`
- Full GitHub Actions expression language (`${{ }}`) evaluator
- Job matrix expansion with `include`/`exclude`
- `needs:` dependency graph with topological wave scheduling
- `uses:` composite and JavaScript actions, `actions/checkout@v4`
- `actions/cache` / artifact API via built-in libmicrohttpd server
- `if:` conditions on steps and jobs, `continue-on-error`, `fail-fast`
- `$GITHUB_OUTPUT` / `$GITHUB_ENV` / `$GITHUB_PATH` protocol
- Secret masking in logs, `hashFiles()` with SHA-256
- Watchdog process: auto-restart on crash with exponential back-off
- Haiku `launch_daemon` service descriptor
- `act_runner run` subcommand: local workflow execution without a server
- 14 test suites, 200+ unit and integration tests
- Haiku-specific: `find_directory()` paths, `load_image()` spawn,
  `sigaction()` signal handling, no epoll/kqueue/signalfd
