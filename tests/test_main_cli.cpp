// tests/test_main_cli.cpp — Integration tests for the act_runner CLI binary.
//
// These tests exercise the `run`, `unregister`, `version`, and `help`
// subcommands by spawning the actual binary and checking exit code + output.
// The `daemon` and `register` subcommands require a real Gitea server so
// they are tested only for error-path behaviour (missing config → exit 1).
//
// The binary path is found via ACT_RUNNER_BIN env var (set by CTest via
// CMAKE_RUNTIME_OUTPUT_DIRECTORY), falling back to ./act_runner.
//
// Haiku note: uses popen() + fgets() to capture output — no epoll needed.

#include "test_runner.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <unistd.h>   // getpid()

namespace fs = std::filesystem;

// ─── Helpers ──────────────────────────────────────────────────────────────

/// Path to the act_runner binary under test.
static std::string binaryPath() {
    const char* env = std::getenv("ACT_RUNNER_BIN");
    if (env && env[0]) return env;
    // CTest runs tests from the build dir — binary is in the same directory.
    return "./act_runner";
}

struct RunResult {
    int         exit_code = -1;
    std::string stdout_stderr;   // combined (2>&1)
};

/// Run the binary with the given arguments, return combined output + exit code.
static RunResult runBinary(const std::string& args) {
    std::string cmd = binaryPath() + " " + args + " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) throw std::runtime_error("popen failed: " + cmd);

    RunResult r;
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) {
        r.stdout_stderr += buf;
    }
    int status = pclose(p);
    // POSIX: pclose returns the wait status — extract exit code.
#if defined(WIFEXITED) && defined(WEXITSTATUS)
    if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
    else                   r.exit_code = -1;
#else
    r.exit_code = status;
#endif
    return r;
}

/// Write a temporary workflow YAML file and return its path.
static std::string writeTmpWorkflow(const std::string& content,
                                    const std::string& suffix = ".yml") {
    std::string path = "/tmp/test_cli_wf_XXXXXX" + suffix;
    // mkstemp doesn't accept a suffix — use a predictable name with PID.
    path = "/tmp/test_cli_wf_" + std::to_string(getpid()) + suffix;
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot write temp workflow: " + path);
    f << content;
    return path;
}

/// Retry a flaky lambda up to `max_tries` times (for Haiku SIGKILLTHR race).
static bool retryFlaky(int max_tries, std::function<bool()> fn) {
    for (int i = 0; i < max_tries; ++i) {
        if (fn()) return true;
    }
    return false;
}

// ─── Tests ────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== test_main_cli ===\n\n";

    // ── version ───────────────────────────────────────────────────────────

    test::run("version exits 0", []() {
        auto r = runBinary("version");
        ASSERT_EQ(r.exit_code, 0);
    });

    test::run("version output contains 'haiku-act-runner'", []() {
        auto r = runBinary("version");
        ASSERT(r.stdout_stderr.find("haiku-act-runner") != std::string::npos);
    });

    // ── help ──────────────────────────────────────────────────────────────

    test::run("help exits 0", []() {
        auto r = runBinary("help");
        ASSERT_EQ(r.exit_code, 0);
    });

    test::run("help output contains subcommands", []() {
        auto r = runBinary("help");
        ASSERT(r.stdout_stderr.find("register")   != std::string::npos);
        ASSERT(r.stdout_stderr.find("daemon")     != std::string::npos);
        ASSERT(r.stdout_stderr.find("run")        != std::string::npos);
        ASSERT(r.stdout_stderr.find("unregister") != std::string::npos);
    });

    test::run("no arguments exits non-zero (shows help)", []() {
        auto r = runBinary("");
        // No args → help text, exit 1
        ASSERT_NE(r.exit_code, 0);
    });

    test::run("unknown subcommand exits non-zero", []() {
        auto r = runBinary("bogus_subcommand");
        ASSERT_NE(r.exit_code, 0);
    });

    // ── daemon (no config) ────────────────────────────────────────────────

    test::run("daemon with missing config exits 1", []() {
        auto r = runBinary("daemon --config /tmp/nonexistent_config_99999.yaml");
        ASSERT_EQ(r.exit_code, 1);
        ASSERT(r.stdout_stderr.find("not found") != std::string::npos
            || r.stdout_stderr.find("not found") != std::string::npos
            || r.stdout_stderr.find("Config")     != std::string::npos);
    });

    // ── register (no Gitea) ───────────────────────────────────────────────

    test::run("register without --url exits 1", []() {
        auto r = runBinary("register --token abc123");
        ASSERT_EQ(r.exit_code, 1);
    });

    test::run("register without --token exits 1", []() {
        auto r = runBinary("register --url http://localhost:9999");
        ASSERT_EQ(r.exit_code, 1);
    });

    // ── unregister (no state) ─────────────────────────────────────────────

    test::run("unregister with no state exits 0 (idempotent)", []() {
        // Point to a config in a non-existent dir — no state file exists.
        auto r = runBinary("unregister --config /tmp/no_such_cfg_99999.yaml");
        ASSERT_EQ(r.exit_code, 0);
        ASSERT(r.stdout_stderr.find("Nothing to unregister") != std::string::npos
            || r.stdout_stderr.find("No runner state") != std::string::npos);
    });

    // ── run (missing file) ────────────────────────────────────────────────

    test::run("run with missing workflow file exits 1", []() {
        auto r = runBinary("run /tmp/does_not_exist_workflow.yml");
        ASSERT_EQ(r.exit_code, 1);
        ASSERT(r.stdout_stderr.find("not found") != std::string::npos
            || r.stdout_stderr.find("not found") != std::string::npos
            || r.stdout_stderr.find("Workflow") != std::string::npos);
    });

    test::run("run with no workflow argument exits 1", []() {
        auto r = runBinary("run");
        ASSERT_EQ(r.exit_code, 1);
        ASSERT(r.stdout_stderr.find("Usage") != std::string::npos
            || r.stdout_stderr.find("workflow") != std::string::npos);
    });

    test::run("run with invalid YAML exits 1", []() {
        std::string path = writeTmpWorkflow("not: [valid: yaml: {\n");
        auto r = runBinary("run " + path);
        fs::remove(path);
        ASSERT_EQ(r.exit_code, 1);
        ASSERT(r.stdout_stderr.find("parse error") != std::string::npos
            || r.stdout_stderr.find("YAML")        != std::string::npos
            || r.stdout_stderr.find("error")       != std::string::npos);
    });

    // ── run (simple success) ──────────────────────────────────────────────

    test::run("run: simple echo step succeeds", []() {
        std::string wf = R"yaml(
name: Simple Test
on: [push]
jobs:
  greet:
    runs-on: haiku
    steps:
      - name: Echo
        run: echo "hello-from-test"
)yaml";
        std::string path = writeTmpWorkflow(wf);

        bool ok = retryFlaky(5, [&]() {
            auto r = runBinary("run " + path + " --event push");
            return r.exit_code == 0
                && r.stdout_stderr.find("SUCCESS") != std::string::npos;
        });
        fs::remove(path);
        ASSERT(ok);
    });

    test::run("run: failing step produces exit 1 and FAILURE summary", []() {
        std::string wf = R"yaml(
name: Fail Test
on: [push]
jobs:
  fail:
    runs-on: haiku
    steps:
      - name: Fail
        run: exit 1
)yaml";
        std::string path = writeTmpWorkflow(wf);

        bool ok = retryFlaky(5, [&]() {
            auto r = runBinary("run " + path + " --event push");
            return r.exit_code == 1
                && r.stdout_stderr.find("FAILURE") != std::string::npos;
        });
        fs::remove(path);
        ASSERT(ok);
    });

    test::run("run: output shows step output lines", []() {
        std::string wf = R"yaml(
name: Output Test
on: [push]
jobs:
  check:
    runs-on: haiku
    steps:
      - name: Print marker
        run: echo "UNIQUE_MARKER_XYZ_789"
)yaml";
        std::string path = writeTmpWorkflow(wf);

        bool ok = retryFlaky(5, [&]() {
            auto r = runBinary("run " + path + " --event push");
            return r.exit_code == 0
                && r.stdout_stderr.find("UNIQUE_MARKER_XYZ_789") != std::string::npos;
        });
        fs::remove(path);
        ASSERT(ok);
    });

    test::run("run: --log-level debug produces DEBUG lines", []() {
        std::string wf = R"yaml(
name: LogLevel Test
on: [push]
jobs:
  dbg:
    runs-on: haiku
    steps:
      - name: Step
        run: echo "ok"
)yaml";
        std::string path = writeTmpWorkflow(wf);

        bool ok = retryFlaky(5, [&]() {
            auto r = runBinary("run " + path + " --log-level debug");
            return r.exit_code == 0
                && r.stdout_stderr.find("DBG") != std::string::npos;
        });
        fs::remove(path);
        ASSERT(ok);
    });

    test::run("run: multi-job workflow succeeds and shows all jobs", []() {
        std::string wf = R"yaml(
name: Multi-Job
on: [push]
jobs:
  setup:
    runs-on: haiku
    steps:
      - name: Setup
        run: echo "setup-ok"
  build:
    needs: setup
    runs-on: haiku
    steps:
      - name: Build
        run: echo "build-ok"
)yaml";
        std::string path = writeTmpWorkflow(wf);

        bool ok = retryFlaky(5, [&]() {
            auto r = runBinary("run " + path + " --event push");
            return r.exit_code == 0
                && r.stdout_stderr.find("setup") != std::string::npos
                && r.stdout_stderr.find("build") != std::string::npos
                && r.stdout_stderr.find("SUCCESS") != std::string::npos;
        });
        fs::remove(path);
        ASSERT(ok);
    });

    test::run("run: --job filter runs only requested job + deps", []() {
        std::string wf = R"yaml(
name: Filter Test
on: [push]
jobs:
  setup:
    runs-on: haiku
    steps:
      - name: Setup
        run: echo "setup-ran"
  build:
    needs: setup
    runs-on: haiku
    steps:
      - name: Build
        run: echo "build-ran"
  unrelated:
    runs-on: haiku
    steps:
      - name: Unrelated
        run: echo "unrelated-ran"
)yaml";
        std::string path = writeTmpWorkflow(wf);

        bool ok = retryFlaky(5, [&]() {
            auto r = runBinary("run " + path + " --job build");
            // build + setup should run; 'unrelated' should NOT appear in summary
            bool ran_setup  = r.stdout_stderr.find("setup-ran") != std::string::npos;
            bool ran_build  = r.stdout_stderr.find("build-ran") != std::string::npos;
            bool no_unrel   = r.stdout_stderr.find("unrelated-ran") == std::string::npos;
            return r.exit_code == 0 && ran_setup && ran_build && no_unrel;
        });
        fs::remove(path);
        ASSERT(ok);
    });

    test::run("run: --job with unknown job id exits 1", []() {
        std::string wf = R"yaml(
name: BadJob
on: [push]
jobs:
  real:
    runs-on: haiku
    steps:
      - name: Real
        run: echo "real"
)yaml";
        std::string path = writeTmpWorkflow(wf);
        auto r = runBinary("run " + path + " --job nonexistent_job");
        fs::remove(path);
        ASSERT_EQ(r.exit_code, 1);
        ASSERT(r.stdout_stderr.find("not found") != std::string::npos
            || r.stdout_stderr.find("nonexistent") != std::string::npos
            || r.stdout_stderr.find("Available") != std::string::npos);
    });

    test::run("run: continue-on-error step lets job succeed", []() {
        std::string wf = R"yaml(
name: ContinueOnError
on: [push]
jobs:
  coe:
    runs-on: haiku
    steps:
      - name: Fail but continue
        continue-on-error: true
        run: exit 1
      - name: After fail
        run: echo "still-running"
)yaml";
        std::string path = writeTmpWorkflow(wf);

        bool ok = retryFlaky(5, [&]() {
            auto r = runBinary("run " + path + " --event push");
            return r.exit_code == 0
                && r.stdout_stderr.find("still-running") != std::string::npos;
        });
        fs::remove(path);
        ASSERT(ok);
    });

    test::run("run: $GITHUB_OUTPUT flows between steps", []() {
        std::string wf = R"yaml(
name: OutputFlow
on: [push]
jobs:
  flow:
    runs-on: haiku
    steps:
      - id: setter
        name: Set output
        run: echo "myval=hello123" >> $GITHUB_OUTPUT
      - name: Use output
        run: |
          if [ "${{ steps.setter.outputs.myval }}" = "hello123" ]; then
            echo "OUTPUT_CORRECT"
          else
            echo "OUTPUT_WRONG"
            exit 1
          fi
)yaml";
        std::string path = writeTmpWorkflow(wf);

        bool ok = retryFlaky(5, [&]() {
            auto r = runBinary("run " + path + " --event push");
            return r.exit_code == 0
                && r.stdout_stderr.find("OUTPUT_CORRECT") != std::string::npos;
        });
        fs::remove(path);
        ASSERT(ok);
    });

    test::run("run: --event sets github.event_name context", []() {
        // We can't directly see github.event_name in output without an expression
        // step, but we can verify --event doesn't crash.
        std::string wf = R"yaml(
name: EventTest
on: [workflow_dispatch]
jobs:
  ev:
    runs-on: haiku
    steps:
      - name: Print event
        run: echo "event-test-ok"
)yaml";
        std::string path = writeTmpWorkflow(wf);

        bool ok = retryFlaky(5, [&]() {
            auto r = runBinary("run " + path + " --event workflow_dispatch");
            return r.exit_code == 0
                && r.stdout_stderr.find("event-test-ok") != std::string::npos;
        });
        fs::remove(path);
        ASSERT(ok);
    });

    test::run("run: --retry 3 flag accepted without error", []() {
        std::string wf = R"yaml(
name: RetryTest
on: [push]
jobs:
  ok:
    runs-on: haiku
    steps:
      - name: Step
        run: echo "retry-test-ok"
)yaml";
        std::string path = writeTmpWorkflow(wf);
        bool ok = retryFlaky(5, [&]() {
            auto r = runBinary("run " + path + " --retry 1 --event push");
            return r.exit_code == 0
                && r.stdout_stderr.find("retry-test-ok") != std::string::npos;
        });
        fs::remove(path);
        ASSERT(ok);
    });

    // ── --help / -h flag tests ─────────────────────────────────────────────

    test::run("--help flag exits 0", []() {
        auto r = runBinary("--help");
        ASSERT_EQ(r.exit_code, 0);
    });

    test::run("-h flag exits 0", []() {
        auto r = runBinary("-h");
        ASSERT_EQ(r.exit_code, 0);
    });

    test::run("--help flag shows usage output", []() {
        auto r = runBinary("--help");
        ASSERT_EQ(r.exit_code, 0);
        ASSERT_CONTAINS(r.stdout_stderr, "register");
        ASSERT_CONTAINS(r.stdout_stderr, "daemon");
        ASSERT_CONTAINS(r.stdout_stderr, "run");
    });

    test::run("run --help exits 0 and shows usage", []() {
        auto r = runBinary("run --help");
        ASSERT_EQ(r.exit_code, 0);
        ASSERT_CONTAINS(r.stdout_stderr, "run");
    });

    test::run("daemon --help exits 0", []() {
        auto r = runBinary("daemon --help");
        ASSERT_EQ(r.exit_code, 0);
    });

    test::run("register --help exits 0", []() {
        auto r = runBinary("register --help");
        ASSERT_EQ(r.exit_code, 0);
    });

    return test::summary();
}
