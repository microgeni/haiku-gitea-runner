// ProcessSpawner.cpp — posix_spawn-based process execution
#include "ProcessSpawner.h"

#include <spawn.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <thread>
#include <atomic>

// POSIX: environ is the current process's environment
extern char** environ;

namespace runner {

// ─── buildEnv ─────────────────────────────────────────────────────────────

std::vector<std::string> buildEnv(
    const std::vector<std::pair<std::string,std::string>>& additions,
    const std::vector<std::string>& removals)
{
    // Start with current environment
    std::vector<std::string> env;
    for (char** e = environ; *e; ++e) {
        env.emplace_back(*e);
    }

    // Remove keys that should be hidden
    for (auto& r : removals) {
        std::string prefix = r + "=";
        env.erase(std::remove_if(env.begin(), env.end(),
            [&prefix](const std::string& e) {
                return e.substr(0, prefix.size()) == prefix;
            }), env.end());
    }

    // Add/override with new values
    for (auto& [k, v] : additions) {
        std::string entry = k + "=" + v;
        std::string prefix = k + "=";
        // Replace existing or append
        bool found = false;
        for (auto& e : env) {
            if (e.substr(0, prefix.size()) == prefix) {
                e = entry;
                found = true;
                break;
            }
        }
        if (!found) {
            env.push_back(entry);
        }
    }

    return env;
}

// ─── drainOutput ──────────────────────────────────────────────────────────

void ProcessSpawner::drainOutput(int fd, bool is_stderr, const OutputCallback& cb) {
    // Read until EOF, calling cb for each complete line.
    std::string buf;
    char tmp[4096];

    while (true) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n <= 0) break;  // EOF or error

        buf.append(tmp, n);

        // Extract complete lines
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            // Strip trailing \r (Windows line endings in scripts)
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (cb) cb(line, is_stderr);
            buf.erase(0, pos + 1);
        }
    }

    // Flush any remaining partial line
    if (!buf.empty() && cb) {
        cb(buf, is_stderr);
    }
}

// ─── ProcessSpawner::run ──────────────────────────────────────────────────

ProcessResult ProcessSpawner::run(
    const std::string&              shell,
    const std::string&              script,
    const std::string&              work_dir,
    const std::vector<std::string>& env,
    OutputCallback                  on_output,
    int                             timeout_s)
{
    ProcessResult result;

    // ── Create pipes ────────────────────────────────────────────────────────
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};

    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        throw std::runtime_error(std::string("pipe() failed: ") + strerror(errno));
    }

    // ── Build argv ────────────────────────────────────────────────────────
    // If 'script' is an absolute path to a file, exec it directly:
    //   argv = [shell, script_path, nullptr]
    // Otherwise treat it as an inline script string via -c:
    //   argv = [shell, "-c", script_string, nullptr]
    std::vector<char*> argv;
    std::string shell_s  = shell;
    std::string dash_c   = "-c";
    std::string script_s = script;
    argv.push_back(shell_s.data());

    bool is_file = !script.empty() && script[0] == '/';
    if (!is_file) {
        argv.push_back(dash_c.data());
    }
    argv.push_back(script_s.data());
    argv.push_back(nullptr);

    // ── Build envp ───────────────────────────────────────────────────────
    std::vector<char*> envp;
    envp.reserve(env.size() + 1);
    for (auto& e : env) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);

    // ── Set up posix_spawn file actions ─────────────────────────────────
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    // Redirect stdout to write end of stdout_pipe
    posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[1]);

    // Redirect stderr to write end of stderr_pipe
    posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, stderr_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, stderr_pipe[1]);

    // Stdin from /dev/null
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                     "/dev/null", O_RDONLY, 0);

    // ── Spawn ────────────────────────────────────────────────────────────
    // posix_spawn doesn't have a built-in chdir action in POSIX.
    // Strategy:
    //   - For file scripts: use posix_spawn_file_actions_addchdir_np if
    //     available (glibc 2.29+, Haiku), otherwise prepend "cd work_dir &&"
    //     as a wrapper (only possible for inline scripts, not file scripts).
    //   - For inline scripts: wrap with "cd work_dir && script"
    std::string wrapped_script;
    bool used_chdir_action = false;

#if defined(HAVE_POSIX_SPAWN_CHDIR) || defined(__linux__) || defined(__HAIKU__)
    // Try to use addchdir_np — if the platform supports it this is the
    // correct way. We detect support at compile time via the macro.
    // (Note: actual availability is platform-dependent; we fall through
    //  to the wrapper approach if it's not available.)
#endif

    if (!work_dir.empty()) {
        if (is_file) {
            // For file scripts: we must chdir the child to work_dir.
            // Use the posix_spawn chdir action (POSIX.1-2024 / glibc 2.29+).
            // If not available, wrap: sh -c "cd <dir> && exec <script_path>"
#ifdef _POSIX_SPAWN_CHDIR
            posix_spawn_file_actions_addchdir_np(&actions, work_dir.c_str());
            used_chdir_action = true;
#else
            // Fallback: switch to -c mode with a cd prefix
            // Rebuild argv as: [shell, "-c", "cd <dir> && exec <script>"]
            argv.clear();
            argv.push_back(shell_s.data());
            argv.push_back(dash_c.data());
            wrapped_script = "cd " + work_dir + " && exec " + script_s;
            argv.push_back(wrapped_script.data());
            argv.push_back(nullptr);
#endif
        } else {
            // For inline -c scripts: prepend cd to the script string
            wrapped_script = "cd " + work_dir + " && " + script_s;
            argv.back() = nullptr;  // was nullptr sentinel
            argv[argv.size() - 2] = wrapped_script.data();  // replace script arg
            // But argv layout was [shell, -c, script, nullptr] — fix properly:
            argv.clear();
            argv.push_back(shell_s.data());
            argv.push_back(dash_c.data());
            argv.push_back(wrapped_script.data());
            argv.push_back(nullptr);
        }
    }
    (void)used_chdir_action;

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);

#ifdef POSIX_SPAWN_SETPGROUP
    // Put the child in its own process group so we can signal
    // the entire process group (child + any grandchildren) on timeout/cancel.
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0); // pgroup = child's own PID
#endif

    pid_t pid = -1;
    int spawn_err = posix_spawn(&pid, shell.c_str(), &actions, &attr,
                                argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);

    // Close write ends in parent
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    if (spawn_err != 0) {
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        throw std::runtime_error(
            std::string("posix_spawn() failed: ") + strerror(spawn_err));
    }

    child_pid_.store(pid);

    // ── Drain output in threads ──────────────────────────────────────────
    std::thread stdout_thread([&]() {
        drainOutput(stdout_pipe[0], false, on_output);
        close(stdout_pipe[0]);
    });
    std::thread stderr_thread([&]() {
        drainOutput(stderr_pipe[0], true, on_output);
        close(stderr_pipe[0]);
    });

    // ── Timeout watchdog ─────────────────────────────────────────────────
    std::thread timeout_thread;
    std::atomic<bool> done{false};
    if (timeout_s > 0) {
        timeout_thread = std::thread([&]() {
            for (int i = 0; i < timeout_s * 10; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (done.load()) return;
            }
            if (!done.load()) {
                // Send SIGTERM to the entire process group (kills child + grandchildren)
                pid_t grp = child_pid_.load();
                if (grp > 0) ::kill(-grp, SIGTERM);
                // Wait up to 5 seconds, then escalate to SIGKILL
                for (int i = 0; i < 50 && !done.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                grp = child_pid_.load();
                if (!done.load() && grp > 0) ::kill(-grp, SIGKILL);
            }
        });
    }

    // ── Wait for child ───────────────────────────────────────────────────
    stdout_thread.join();
    stderr_thread.join();

    int wstatus = 0;
    pid_t waited = waitpid(pid, &wstatus, 0);
    done.store(true);

    if (timeout_thread.joinable()) timeout_thread.join();

    child_pid_.store(-1);

    if (waited == -1) {
        throw std::runtime_error(std::string("waitpid() failed: ") + strerror(errno));
    }

    if (WIFEXITED(wstatus)) {
        result.exit_code = WEXITSTATUS(wstatus);
    } else if (WIFSIGNALED(wstatus)) {
        result.killed   = true;
        result.signal   = WTERMSIG(wstatus);
        result.exit_code = 128 + result.signal;
    }

    return result;
}

// ─── ProcessSpawner::kill ─────────────────────────────────────────────────

void ProcessSpawner::kill() {
    pid_t pid = child_pid_.load();
    if (pid > 0) {
        // Kill the entire process group (child + grandchildren)
        if (::kill(-pid, SIGTERM) != 0) {
            // Fallback if process group not set
            ::kill(pid, SIGTERM);
        }
    }
}

} // namespace runner
