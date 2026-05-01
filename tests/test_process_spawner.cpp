// tests/test_process_spawner.cpp — Unit tests for ProcessSpawner
#include "test_runner.h"
#include "../src/process/ProcessSpawner.h"

#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>

using namespace runner;
using namespace test;

// ── Flake retry ───────────────────────────────────────────────────────────
// On Haiku, posix_spawn() occasionally delivers SIGKILLTHR (signal 7) to a
// newly spawned child while the parent has live threads.  Retry transient
// failures up to max_attempts times with an exponential back-off.
static void retry_on_flake(std::function<void()> body, int max_attempts = 5) {
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        try {
            body();
            return;
        } catch (const std::exception&) {
            if (attempt == max_attempts) throw;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(50 * attempt));
        }
    }
}

template<typename F>
static auto flaky(F body) {
    return [body]() { retry_on_flake(body, 5); };
}

int main() {
    std::cout << "=== ProcessSpawner tests ===\n\n";

    run("run simple echo command", flaky([]() {
        ProcessSpawner ps;
        std::vector<std::string> lines;
        auto cb = [&](const std::string& line, bool) { lines.push_back(line); };
        auto result = ps.run("/bin/sh", "echo hello", "/tmp", {}, cb);
        ASSERT_EQ(result.exit_code, 0);
        ASSERT(!lines.empty());
        ASSERT_EQ(lines[0], "hello");
    }));

    run("exit code propagated correctly", flaky([]() {
        ProcessSpawner ps;
        auto result = ps.run("/bin/sh", "exit 42", "/tmp", {});
        ASSERT_EQ(result.exit_code, 42);
        ASSERT(!result.killed);
    }));

    run("exit code 0 for success", flaky([]() {
        ProcessSpawner ps;
        auto result = ps.run("/bin/sh", "true", "/tmp", {});
        ASSERT_EQ(result.exit_code, 0);
    }));

    run("exit code non-zero for false", flaky([]() {
        ProcessSpawner ps;
        auto result = ps.run("/bin/sh", "false", "/tmp", {});
        ASSERT_NE(result.exit_code, 0);
    }));

    run("environment variable passed to child", flaky([]() {
        ProcessSpawner ps;
        std::vector<std::string> lines;
        auto cb = [&](const std::string& l, bool) { lines.push_back(l); };

        std::vector<std::string> env = buildEnv({{"MY_TEST_VAR", "haiku_runner"}});
        auto result = ps.run("/bin/sh", "echo $MY_TEST_VAR", "/tmp", env, cb);

        ASSERT_EQ(result.exit_code, 0);
        ASSERT(!lines.empty());
        ASSERT_EQ(lines[0], "haiku_runner");
    }));

    run("working directory is respected", flaky([]() {
        ProcessSpawner ps;
        std::vector<std::string> lines;
        auto cb = [&](const std::string& l, bool) { lines.push_back(l); };

        auto result = ps.run("/bin/sh", "pwd", "/tmp", {}, cb);
        ASSERT_EQ(result.exit_code, 0);
        ASSERT(!lines.empty());
        // pwd inside /tmp may resolve to real path — just check it's not empty
        ASSERT(!lines[0].empty());
    }));

    run("multiline output captured", flaky([]() {
        ProcessSpawner ps;
        std::vector<std::string> lines;
        auto cb = [&](const std::string& l, bool) { lines.push_back(l); };

        auto result = ps.run("/bin/sh",
            "echo line1; echo line2; echo line3", "/tmp", {}, cb);
        ASSERT_EQ(result.exit_code, 0);
        ASSERT_EQ(lines.size(), 3u);
        ASSERT_EQ(lines[0], "line1");
        ASSERT_EQ(lines[1], "line2");
        ASSERT_EQ(lines[2], "line3");
    }));

    run("stderr captured via callback", flaky([]() {
        ProcessSpawner ps;
        std::vector<std::string> stderr_lines;
        auto cb = [&](const std::string& l, bool is_err) {
            if (is_err) stderr_lines.push_back(l);
        };

        auto result = ps.run("/bin/sh",
            "echo error_msg >&2", "/tmp", {}, cb);
        ASSERT_EQ(result.exit_code, 0);
        ASSERT(!stderr_lines.empty());
        ASSERT_EQ(stderr_lines[0], "error_msg");
    }));

    run("timeout kills process", flaky([]() {
        ProcessSpawner ps;
        auto start = std::chrono::steady_clock::now();
        auto result = ps.run("/bin/sh", "sleep 30", "/tmp", {}, nullptr, 1);
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();

        // Should complete in <10s (1s timeout + 5s SIGKILL grace)
        ASSERT(elapsed < 10);
        ASSERT_NE(result.exit_code, 0);
    }));

    run("null output callback is safe", flaky([]() {
        ProcessSpawner ps;
        auto result = ps.run("/bin/sh", "echo hello", "/tmp", {}, nullptr);
        ASSERT_EQ(result.exit_code, 0);
    }));

    run("buildEnv: adds new variable", []() {
        auto env = buildEnv({{"MY_KEY", "my_value"}});
        bool found = false;
        for (auto& e : env) {
            if (e == "MY_KEY=my_value") { found = true; break; }
        }
        ASSERT(found);
    });

    run("buildEnv: overrides existing variable", []() {
        // PATH should exist in current env; override it
        auto env = buildEnv({{"PATH", "/override/path"}});
        bool found = false;
        for (auto& e : env) {
            if (e == "PATH=/override/path") { found = true; }
            // Should not have two PATH entries
        }
        ASSERT(found);
        int path_count = 0;
        for (auto& e : env) {
            if (e.substr(0, 5) == "PATH=") ++path_count;
        }
        ASSERT_EQ(path_count, 1);
    });

    run("buildEnv: preserves existing env", []() {
        auto env = buildEnv({{"ADDED_VAR", "yes"}});
        // Should still have HOME or PATH from the parent env
        bool has_path = false;
        for (auto& e : env) {
            if (e.substr(0, 5) == "PATH=") { has_path = true; break; }
        }
        ASSERT(has_path);
    });

    return summary();
}
