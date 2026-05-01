#pragma once
// ProcessSpawner.h — POSIX process spawning wrapper
//
// Uses posix_spawn() (preferred over fork/exec on Haiku) to launch child
// processes with controlled stdin/stdout/stderr redirects and custom
// environment variables.
//
// Haiku notes:
//   - posix_spawn() is fully supported on Haiku.
//   - waitpid() works as expected.
//   - No epoll/signalfd: we use blocking waitpid() in a dedicated thread.

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <atomic>

namespace runner {

/// Result of a spawned process.
struct ProcessResult {
    int     exit_code = -1;   ///< 0 = success, non-zero = failure
    bool    killed    = false; ///< true if terminated by signal
    int     signal    = 0;     ///< signal number if killed != false
};

/// Callback invoked for each line captured from the child process's output.
/// @param line  line content (without trailing newline)
/// @param is_stderr  true if from stderr, false if stdout
using OutputCallback = std::function<void(const std::string& line, bool is_stderr)>;

/// Spawn and monitor a child process.
class ProcessSpawner {
public:
    ProcessSpawner() = default;
    ~ProcessSpawner() = default;

    // Non-copyable, movable
    ProcessSpawner(const ProcessSpawner&)            = delete;
    ProcessSpawner& operator=(const ProcessSpawner&) = delete;
    ProcessSpawner(ProcessSpawner&&)                 = default;
    ProcessSpawner& operator=(ProcessSpawner&&)      = default;

    /// Execute a command in a shell.
    ///
    /// @param shell      path to shell, e.g. "/bin/sh" or "/bin/bash"
    /// @param script     script text to feed to the shell via -c
    /// @param work_dir   working directory for the child process
    /// @param env        environment variables for the child (key=value pairs)
    /// @param on_output  callback invoked for each output line (may be null)
    /// @param timeout_s  kill the process after this many seconds (0 = no timeout)
    ///
    /// @returns ProcessResult
    ProcessResult run(
        const std::string&              shell,
        const std::string&              script,
        const std::string&              work_dir,
        const std::vector<std::string>& env,
        OutputCallback                  on_output = nullptr,
        int                             timeout_s = 0
    );

    /// Send SIGTERM to the current child process (if any).
    /// Thread-safe.
    void kill();

private:
    std::atomic<pid_t> child_pid_{-1};

    // Reads lines from fd, calls cb for each.
    static void drainOutput(int fd, bool is_stderr, const OutputCallback& cb);
};

/// Build a complete environment variable array for a child process.
/// Starts with the current process's env and applies overrides/additions.
std::vector<std::string> buildEnv(
    const std::vector<std::pair<std::string,std::string>>& additions,
    const std::vector<std::string>& removals = {}
);

} // namespace runner
