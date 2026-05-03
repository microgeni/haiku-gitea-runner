// ProcessSpawner.cpp — Process spawning wrapper
//
// On Haiku we use load_image() + resume_thread() instead of posix_spawn().
//
// WHY: Haiku's posix_spawn() is implemented via an intermediate "spawner
// thread" that is created inside the parent team.  When that spawner thread
// exits (after a successful load_image() + resume_thread()), the kernel's
// thread-exit path can incorrectly deliver SIGKILLTHR (signal 21, the
// Haiku-private "kill this thread" signal) to the parent team's main thread,
// killing the entire daemon process.  SIGKILLTHR cannot be caught, blocked,
// or ignored from user space, so the only correct fix is to avoid
// posix_spawn() on Haiku altogether.
//
// load_image() is a single kernel syscall that creates a new team directly,
// with the child's main thread starting suspended.  No intermediate thread is
// created inside the parent team, so SIGKILLTHR cannot fire from this path.
//
// Reference: Haiku bug #18708; src/system/libroot/posix/spawn/posix_spawn.cpp
// in the Haiku source tree.

#include "ProcessSpawner.h"

#ifdef __HAIKU__
#  include <image.h>   // load_image(), resume_thread()
#  include <OS.h>      // thread_id, B_OK
#endif

#include <spawn.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <mutex>

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
        ssize_t n;
        do {
            n = read(fd, tmp, sizeof(tmp));
        } while (n < 0 && errno == EINTR);  // retry on signal interruption

        if (n <= 0) break;  // EOF or unrecoverable error

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
    // Use O_CLOEXEC so that pipe fds are automatically closed in any
    // load_image() / exec'd child that doesn't explicitly inherit them.
    // The write ends are dup2'd to STDOUT/STDERR (which clears O_CLOEXEC on
    // the dup), giving the child exactly one copy of each write end.
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};

    if (pipe2(stdout_pipe, O_CLOEXEC) != 0 || pipe2(stderr_pipe, O_CLOEXEC) != 0) {
        throw std::runtime_error(std::string("pipe2() failed: ") + strerror(errno));
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

    // ── Set up posix_spawn file actions (non-Haiku only) ─────────────────
    // On Haiku we use load_image() which inherits fds from the parent, so
    // we redirect our own stdout/stderr directly before calling it.
#ifndef __HAIKU__
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
#endif // !__HAIKU__

    // ── Wrap script with work_dir ─────────────────────────────────────────
    std::string wrapped_script;

    if (!work_dir.empty()) {
        if (is_file) {
#if !defined(__HAIKU__) && defined(_POSIX_SPAWN_CHDIR)
            posix_spawn_file_actions_addchdir_np(&actions, work_dir.c_str());
#else
            argv.clear();
            argv.push_back(shell_s.data());
            argv.push_back(dash_c.data());
            wrapped_script = "cd " + work_dir + " && exec " + script_s;
            argv.push_back(wrapped_script.data());
            argv.push_back(nullptr);
#endif
        } else {
            wrapped_script = "cd " + work_dir + " && " + script_s;
            argv.clear();
            argv.push_back(shell_s.data());
            argv.push_back(dash_c.data());
            argv.push_back(wrapped_script.data());
            argv.push_back(nullptr);
        }
    }

#ifndef __HAIKU__
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#  ifdef POSIX_SPAWN_SETPGROUP
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
#  endif
#endif // !__HAIKU__

    pid_t pid = -1;

#ifdef __HAIKU__
    // ── Haiku: use load_image() instead of posix_spawn() ──────────────────
    //
    // load_image() creates the child team directly without an intermediate
    // spawner thread in the parent — avoids the SIGKILLTHR race in
    // posix_spawn().  The child's main thread starts SUSPENDED; we redirect
    // its inherited fds then call resume_thread().
    //
    // Fd manipulation must be serialised: we temporarily dup2 our own
    // stdout/stderr to the pipe write ends so load_image() snapshots the
    // right fds, then restore them immediately after.
    {
        static std::mutex s_load_image_mutex;
        std::lock_guard<std::mutex> lk(s_load_image_mutex);

        // Save parent's stdout/stderr
        int saved_out = ::dup(STDOUT_FILENO);
        int saved_err = ::dup(STDERR_FILENO);
        ::fcntl(saved_out, F_SETFD, FD_CLOEXEC);
        ::fcntl(saved_err, F_SETFD, FD_CLOEXEC);

        // Redirect to pipe write ends
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);

        // Arrange for stdin from /dev/null
        int dev_null = ::open("/dev/null", O_RDONLY);
        int saved_in = -1;
        if (dev_null >= 0) {
            saved_in = ::dup(STDIN_FILENO);
            ::fcntl(saved_in, F_SETFD, FD_CLOEXEC);
            ::dup2(dev_null, STDIN_FILENO);
            ::close(dev_null);
        }

        // load_image() — single kernel syscall, no spawner thread created
        thread_id child_tid = ::load_image(
            static_cast<int>(argv.size() - 1),
            const_cast<const char**>(argv.data()),
            const_cast<const char**>(envp.data())
        );

        // Restore parent fds immediately
        ::dup2(saved_out, STDOUT_FILENO);
        ::dup2(saved_err, STDERR_FILENO);
        if (saved_in >= 0) { ::dup2(saved_in, STDIN_FILENO); ::close(saved_in); }
        ::close(saved_out);
        ::close(saved_err);

        if (child_tid < B_OK) {
            ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
            ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
            throw std::runtime_error(
                std::string("load_image() failed: ") + strerror((int)-child_tid));
        }

        // Set the child's process group (mirrors POSIX_SPAWN_SETPGROUP)
        // setpgid() on the child team works before resume_thread()
        ::setpgid(static_cast<pid_t>(child_tid), 0);

        // Resume the child's main thread
        ::resume_thread(child_tid);

        pid = static_cast<pid_t>(child_tid);
    }

#else
    // ── POSIX fallback: posix_spawn() ─────────────────────────────────────
    int spawn_err = posix_spawn(&pid, shell.c_str(), &actions, &attr,
                                argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (spawn_err != 0) {
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
        throw std::runtime_error(
            std::string("posix_spawn() failed: ") + strerror(spawn_err));
    }
#endif

    // Close write ends in parent (child has them via inheritance / dup2)
    ::close(stdout_pipe[1]); stdout_pipe[1] = -1;
    ::close(stderr_pipe[1]); stderr_pipe[1] = -1;

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
