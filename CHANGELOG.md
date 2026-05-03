# Changelog

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
