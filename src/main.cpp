// main.cpp — haiku-act-runner entry point
//
// CLI interface:
//   act_runner register   --url <gitea_url> --token <reg_token> [--name <name>]
//                         [--labels <l1,l2>] [--config <path>] [--insecure]
//   act_runner daemon     [--config <path>] [--log-level <level>]
//   act_runner run        <workflow.yml> [--event <name>] [--payload <json>]
//                         [--job <id>] [--log-level <level>]
//   act_runner unregister [--config <path>]
//   act_runner version
//   act_runner help
//
// On Haiku:
//   Settings stored in B_USER_SETTINGS_DIRECTORY/act_runner/
//   Logs written to stdout/stderr (redirect via launch_daemon service)

#include "config/Config.h"
#include "config/RunnerState.h"
#include "client/GiteaClient.h"
#include "client/RunnerClient.h"
#include "client/LocalRunnerClient.h"
#include "runner/Poller.h"
#include "runner/WorkflowOrchestrator.h"
#include "workflow/WorkflowParser.h"
#include "util/Logger.h"

#ifdef HAVE_MICROHTTPD
#  include "cache/CacheServer.h"
#endif

#include <iostream>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>
#include <filesystem>
#include <exception>
#include <setjmp.h>   // sigjmp_buf, siglongjmp
#include <unistd.h>
#include <sys/wait.h> // waitpid
#include <mutex>
#include <curl/curl.h>

extern char** environ;  // current process environment (used by watchdog)

#ifdef __HAIKU__
#  include <FindDirectory.h>
#  include <Path.h>
#endif

// ─── Global shutdown flag ──────────────────────────────────────────────────

static std::atomic<bool> g_shutdown{false};

static void signal_handler(int /*sig*/) {
    g_shutdown.store(true);
}

// Signal handler for Haiku-internal signals (SIGTRAP=5 from load_image,
// SIGUSR2=12 from the debug server) that would otherwise kill the daemon.
// We simply return so the daemon continues running.
// SIGSEGV handler: longjmp out of the faulting task execution.
// Both run on the alternate signal stack (SA_ONSTACK).

// Thread-local recovery point for worker threads — defined in Poller.cpp,
// used by benign_signal_handler to siglongjmp back to the workerLoop boundary.
extern thread_local sigjmp_buf t_task_recover;
extern thread_local volatile bool t_in_task;

// Signal handler for Haiku-internal signals (SIGCHLD=5 from load_image children
// exiting, SIGTRAP=22 from the debug server) that would otherwise kill the daemon.
// SIGSEGV/SIGBUS inside a task: recovered via siglongjmp back to workerLoop.
// SIGSEGV/SIGBUS outside a task: re-raised with default action (crash).
// Runs on the alternate signal stack (SA_ONSTACK).
static void benign_signal_handler(int sig) {
    if (sig == SIGSEGV || sig == SIGBUS) {
        if (t_in_task) {
            // Recover: jump back to the worker loop boundary.
            siglongjmp(t_task_recover, sig);
        }
        // Not in a task — reinstall default and re-raise for a real crash.
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    // All other signals: swallow and continue.
    // On Haiku:
    //   signal 5  = SIGCHLD (child process exited via load_image)
    //   signal 12 = SIGCONT (continue — harmless)
    //   signal 22 = SIGTRAP (from debug_server on new-team creation)
    // We silently ignore them so the daemon keeps running.
}


// ─── Version ──────────────────────────────────────────────────────────────

#ifndef RUNNER_VERSION
#  define RUNNER_VERSION "0.2.0"
#endif

static const char* VERSION = RUNNER_VERSION;

// Strip surrounding quotes if the macro added them.
static std::string cleanVersion(const char* v) {
    std::string s = v;
    if (!s.empty() && s.front() == '"') s = s.substr(1);
    if (!s.empty() && s.back()  == '"') s.pop_back();
    return s;
}

// ─── Usage ────────────────────────────────────────────────────────────────

static void printUsage(const char* prog) {
    std::cout
        << "haiku-act-runner " << cleanVersion(VERSION) << "\n"
        << "A native Gitea Actions runner for Haiku OS\n\n"
        << "Usage:\n"
        << "  " << prog << " register   [options]           Register this runner with Gitea\n"
        << "  " << prog << " daemon     [options]           Start the runner daemon\n"
        << "  " << prog << " run        <workflow.yml> [opts] Execute a workflow locally\n"
        << "  " << prog << " unregister [options]           Remove this runner from Gitea\n"
        << "  " << prog << " version                        Print version and exit\n"
        << "  " << prog << " help                           Print this help\n\n"
        << "Register options:\n"
        << "  --url      <url>        Gitea server URL (required)\n"
        << "  --token    <token>      Registration token (required)\n"
        << "  --name     <name>       Runner display name (default: hostname)\n"
        << "  --labels   <l1,l2>     Comma-separated labels (default: haiku:host)\n"
        << "  --config   <path>       Config file path\n"
        << "  --insecure              Skip TLS certificate verification\n\n"
        << "Daemon options:\n"
        << "  --config    <path>      Config file path\n"
        << "  --log-level <level>     Log level: debug|info|warn|error (default: info)\n\n"
        << "Run options:\n"
        << "  <workflow.yml>          Path to workflow YAML file (required)\n"
        << "  --event    <name>       Simulated event name (default: push)\n"
        << "  --payload  <json>       Event payload JSON string or @file path\n"
        << "  --job      <id>         Run only the specified job (default: all)\n"
        << "  --retry    <N>          Retry the workflow up to N times on failure\n"
        << "                          (default: 0). Useful on Haiku to work around\n"
        << "                          the SIGKILLTHR posix_spawn race (~10% rate).\n"
        << "  --log-level <level>     Log level: debug|info|warn|error (default: info)\n\n"
        << "Unregister options:\n"
        << "  --config   <path>       Config file path\n\n"
        << "Default config path: " << runner::defaultConfigPath() << "\n\n"
        << "Examples:\n"
        << "  " << prog << " register --url https://gitea.example.com \\\n"
        << "                  --token ABCDEF1234567890 --name haiku-builder\n\n"
        << "  " << prog << " daemon\n\n"
        << "  " << prog << " run .gitea/workflows/ci.yml --event push\n\n"
        << "  " << prog << " unregister\n";
}

// ─── Argument parsing ─────────────────────────────────────────────────────

struct Args {
    std::string command;          // "register"|"daemon"|"run"|"unregister"|"version"|"help"
    bool        implicit_help = false;  // true if no args were given (exit 1)
    std::string config_path;
    // register
    std::string url;
    std::string reg_token;
    std::string name;
    std::vector<std::string> labels;
    bool insecure = false;
    // daemon / run
    std::string log_level;
    bool watchdog_child = false;  // internal: set when spawned by the watchdog wrapper
    // run
    std::string workflow_file;
    std::string event_name;
    std::string event_payload;    // raw JSON or @filename
    std::string job_filter;       // run only this job id
    int         retry_count = 0;  // --retry N (default: no retry)
};

static Args parseArgs(int argc, char* argv[]) {
    Args args;
    if (argc < 2) {
        args.command      = "help";
        args.implicit_help = true;
        return args;
    }

    // Accept 'help' and '--help'/'-h' as the first argument.
    std::string first = argv[1];
    if (first == "--help" || first == "-h") {
        args.command = "help";
        return args;
    }
    args.command = first;

    // The first positional argument after "run" is the workflow file.
    bool first_positional = (args.command == "run");

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];

        // --help / -h anywhere on the command line → show help and exit 0.
        if (a == "--help" || a == "-h") {
            args.command = "help";
            args.implicit_help = false;
            return args;
        }

        // First positional (non-flag) for the run subcommand is the workflow file.
        if (first_positional && a[0] != '-') {
            args.workflow_file = a;
            first_positional = false;
            continue;
        }

        auto nextArg = [&]() -> std::string {
            if (i + 1 < argc) return argv[++i];
            throw std::runtime_error("Expected argument after " + a);
        };

        if      (a == "--config")       args.config_path      = nextArg();
        else if (a == "--url")          args.url               = nextArg();
        else if (a == "--token")        args.reg_token         = nextArg();
        else if (a == "--name")         args.name              = nextArg();
        else if (a == "--insecure")     args.insecure          = true;
        else if (a == "--log-level")    args.log_level         = nextArg();
        else if (a == "--watchdog-child") args.watchdog_child  = true;
        else if (a == "--event")        args.event_name        = nextArg();
        else if (a == "--payload")   args.event_payload  = nextArg();
        else if (a == "--job")       args.job_filter     = nextArg();
        else if (a == "--retry") {
            std::string nstr = nextArg();
            try { args.retry_count = std::stoi(nstr); }
            catch (...) { throw std::runtime_error("--retry requires an integer, got: " + nstr); }
            if (args.retry_count < 0) args.retry_count = 0;
        }
        else if (a == "--labels") {
            std::string raw = nextArg();
            size_t pos = 0, found;
            while ((found = raw.find(',', pos)) != std::string::npos) {
                args.labels.push_back(raw.substr(pos, found - pos));
                pos = found + 1;
            }
            args.labels.push_back(raw.substr(pos));
        }
        else {
            throw std::runtime_error("Unknown option: " + a);
        }
    }
    return args;
}

// ─── hostname helper ──────────────────────────────────────────────────────

static std::string getHostname() {
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) == 0) return buf;
    return "haiku-runner";
}

// ─── Register subcommand ──────────────────────────────────────────────────

static int cmdRegister(const Args& args) {
    if (args.url.empty()) {
        LOG_ERROR("register", "--url is required for registration");
        return 1;
    }
    if (args.reg_token.empty()) {
        LOG_ERROR("register", "--token is required for registration");
        return 1;
    }

    std::string config_path = args.config_path.empty()
                            ? runner::defaultConfigPath()
                            : args.config_path;
    std::string state_path  = runner::defaultRunnerStatePath();

    // Load existing config or create a minimal one.
    runner::Config cfg;
    try {
        cfg = runner::loadConfig(config_path);
    } catch (...) {
        cfg.gitea_url = args.url;
    }
    if (!args.url.empty())   cfg.gitea_url = args.url;
    if (args.insecure)       cfg.insecure  = true;

    std::string runner_name = args.name.empty() ? getHostname() : args.name;

    std::vector<std::string> label_strings;
    if (!args.labels.empty()) {
        label_strings = args.labels;
    } else if (!cfg.labels.empty()) {
        label_strings = cfg.labelStrings();
    } else {
        label_strings = {"haiku:host", "haiku-latest:host"};
    }

    {
        std::string lbls;
        for (auto& l : label_strings) { if (!lbls.empty()) lbls += ' '; lbls += l; }
        LOG_INFO("register", "Registering '" << runner_name
                 << "' with " << cfg.gitea_url << "  labels: " << lbls);
    }

    runner::RunnerClient rpc_client(cfg.gitea_url, cfg.insecure);

    runner::RegisterResult reg;
    try {
        reg = rpc_client.registerRunner(args.reg_token, runner_name, label_strings);
    } catch (const std::exception& e) {
        LOG_ERROR("register", "Registration failed: " << e.what());
        return 1;
    }

    if (reg.runner_token.empty()) {
        LOG_ERROR("register", "Server returned empty token — check Gitea server logs");
        return 1;
    }

    // Declare version + labels so Gitea stores them server-side for task routing.
    // (Required by Gitea 1.21+; the runner token from Register is used here.)
    // Declare takes plain label names (e.g. "haiku"), not "name:type" strings.
    rpc_client.setRunnerToken(reg.runner_token);
    rpc_client.setRunnerUUID(reg.uuid);
    {
        std::vector<std::string> declare_labels;
        for (auto& l : label_strings) {
            auto pos = l.find(':');
            declare_labels.push_back(pos == std::string::npos ? l : l.substr(0, pos));
        }
        try {
            rpc_client.declare(declare_labels);
            LOG_INFO("register", "Declare OK");
        } catch (const std::exception& e) {
            // Non-fatal — older Gitea versions may not have Declare.
            LOG_WARN("register", "Declare RPC failed (non-fatal): " << e.what());
        }
    }

    runner::RunnerState state;
    state.token  = reg.runner_token;
    state.uuid   = reg.uuid;
    state.name   = reg.name.empty() ? runner_name : reg.name;
    state.labels = label_strings;

    try {
        runner::saveRunnerState(state, state_path);
        LOG_INFO("register", "Runner state saved to: " << state_path);
    } catch (const std::exception& e) {
        LOG_ERROR("register", "Failed to save runner state: " << e.what());
        return 1;
    }

    cfg.name         = state.name;
    cfg.runner_token = reg.runner_token;
    try {
        runner::saveConfig(cfg, config_path);
        LOG_INFO("register", "Config saved to: " << config_path);
    } catch (const std::exception& e) {
        LOG_WARN("register", "Failed to save config (non-fatal): " << e.what());
    }

    std::cout << "\nRegistration successful!\n"
              << "  Runner name : " << state.name << "\n"
              << "  Runner UUID : " << state.uuid << "\n"
              << "  Token       : " << state.token.substr(0, 8) << "...\n\n"
              << "Now run:  act_runner daemon\n";
    return 0;
}

// ─── Daemon subcommand ────────────────────────────────────────────────────

// ── Watchdog wrapper ───────────────────────────────────────────────────────
// When invoked as "daemon" (without --watchdog-child), cmdDaemon forks a
// child that runs the actual daemon loop, and acts as a watchdog that
// restarts the child on unexpected exit.  SIGTERM/SIGINT are forwarded to
// the child; if the child exits cleanly (exit 0) the watchdog exits too.
//
// This means the daemon is self-healing even when not managed by
// launch_daemon: if the daemon crashes (e.g. due to SIGKILLTHR), the
// watchdog notices within 1 s and re-execs it automatically.
//
// Child processes are spawned via execv() to inherit the same binary and
// all arguments, with --watchdog-child appended.

static pid_t g_watchdog_child_pid = -1;

static void watchdog_signal_handler(int sig) {
    // Forward the signal to the child, then set our own shutdown flag.
    if (g_watchdog_child_pid > 0) kill(g_watchdog_child_pid, sig);
    g_shutdown.store(true);
}

static int runWatchdog(int argc, char* argv[]) {
    // Build the child argv: copy original argv + "--watchdog-child"
    std::vector<char*> child_argv(argv, argv + argc);
    const char* wc_flag = "--watchdog-child";
    child_argv.push_back(const_cast<char*>(wc_flag));
    child_argv.push_back(nullptr);

    // Pre-flight: if config doesn't exist, exit immediately with code 1
    // (same as the child would do). This avoids restart loops when not registered.
    {
        Args a;
        try { a = parseArgs(argc, argv); } catch (...) {}
        std::string cfg_path = a.config_path.empty()
                               ? runner::defaultConfigPath()
                               : a.config_path;
        if (!std::filesystem::exists(cfg_path)) {
            std::cerr << "Config file not found: " << cfg_path << "\n"
                      << "Run 'act_runner register --url <gitea_url> --token <token>' first.\n";
            return 1;
        }
    }

    // Install our signal handlers on the watchdog side
    struct sigaction sa{};
    sa.sa_handler = watchdog_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Ignore SIGCHLD so we can waitpid() explicitly
    signal(SIGCHLD, SIG_DFL);

    int restart_delay_s = 0;
    int attempts = 0;

    while (!g_shutdown.load()) {
        if (restart_delay_s > 0) {
            LOG_INFO("watchdog", "Restarting daemon in " << restart_delay_s << "s"
                     << " (attempt " << attempts << ")");
            for (int i = 0; i < restart_delay_s && !g_shutdown.load(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (g_shutdown.load()) break;
        }

#ifdef __HAIKU__
        // On Haiku use load_image() to avoid the posix_spawn SIGKILLTHR race
        {
            static std::mutex s_exec_mutex;
            std::lock_guard<std::mutex> lk(s_exec_mutex);

            // Temporarily dup stdout/stderr to /dev/null for the brief window
            // between load_image and resume — the child inherits the real fds.
            thread_id child_tid = ::load_image(
                static_cast<int>(child_argv.size() - 1),
                const_cast<const char**>(child_argv.data()),
                const_cast<const char**>(environ)
            );
            if (child_tid < B_OK) {
                LOG_ERROR("watchdog", "load_image() failed: " << strerror((int)-child_tid));
                restart_delay_s = std::min(restart_delay_s + 5, 60);
                ++attempts;
                continue;
            }
            ::setpgid(static_cast<pid_t>(child_tid), 0);
            g_watchdog_child_pid = static_cast<pid_t>(child_tid);
            ::resume_thread(child_tid);
        }
#else
        {
            pid_t pid = fork();
            if (pid == 0) {
                // Child
                execv("/proc/self/exe", child_argv.data());
                execv(child_argv[0], child_argv.data());
                _exit(127);
            }
            if (pid < 0) {
                LOG_ERROR("watchdog", "fork() failed: " << strerror(errno));
                restart_delay_s = std::min(restart_delay_s + 5, 60);
                ++attempts;
                continue;
            }
            g_watchdog_child_pid = pid;
        }
#endif

        LOG_INFO("watchdog", "Daemon child started (pid " << g_watchdog_child_pid << ")");

        // Wait for the child to exit
        int wstatus = 0;
        while (true) {
            pid_t reaped = waitpid(g_watchdog_child_pid, &wstatus, WNOHANG);
            if (reaped == g_watchdog_child_pid) break;
            if (g_shutdown.load()) {
                // Give child 10s to finish gracefully
                for (int i = 0; i < 10 && kill(g_watchdog_child_pid, 0) == 0; ++i)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                kill(g_watchdog_child_pid, SIGKILL);
                waitpid(g_watchdog_child_pid, &wstatus, 0);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        g_watchdog_child_pid = -1;

        if (g_shutdown.load()) break;

        int exit_code = 0;
        bool crashed  = false;
        if (WIFEXITED(wstatus)) {
            exit_code = WEXITSTATUS(wstatus);
            if (exit_code == 0) {
                LOG_INFO("watchdog", "Daemon exited cleanly — stopping watchdog.");
                return 0;
            }
            LOG_WARN("watchdog", "Daemon exited with code " << exit_code << " — will restart.");
            crashed = true;
        } else if (WIFSIGNALED(wstatus)) {
            int sig = WTERMSIG(wstatus);
            LOG_WARN("watchdog", "Daemon killed by signal " << sig << " — will restart.");
            crashed = true;
        }

        if (crashed) {
            ++attempts;
            // Exponential-ish backoff: 2, 4, 8 … capped at 30s
            restart_delay_s = std::min(2 << std::min(attempts, 4), 30);
        }
    }

    return 0;
}

static int cmdDaemon(const Args& args) {
    std::string config_path = args.config_path.empty()
                            ? runner::defaultConfigPath()
                            : args.config_path;

    if (!std::filesystem::exists(config_path)) {
        std::cerr << "Config file not found: " << config_path << "\n"
                  << "Run 'act_runner register --url <gitea_url> --token <token>' first.\n";
        return 1;
    }

    runner::Config cfg;
    try {
        cfg = runner::loadConfig(config_path);
    } catch (const std::exception& e) {
        std::cerr << "Failed to load config from '" << config_path << "': "
                  << e.what() << "\n";
        return 1;
    }

    if (args.insecure) cfg.insecure = true;

    // CLI --log-level overrides config
    if (!args.log_level.empty()) cfg.log_level = args.log_level;
    runner::Logger::instance().setLevel(cfg.log_level);

    if (cfg.gitea_url.empty()) {
        std::cerr << "Config missing 'gitea_url'. Please re-run 'act_runner register'.\n";
        return 1;
    }

    std::string state_path = runner::defaultRunnerStatePath();
    if (!std::filesystem::exists(state_path)) {
        std::cerr << "Runner state not found: " << state_path << "\n"
                  << "Run 'act_runner register --url <gitea_url> --token <token>' first.\n";
        return 1;
    }

    runner::RunnerState state;
    try {
        state = runner::loadRunnerState(state_path);
    } catch (const std::exception& e) {
        std::cerr << "Failed to load runner state: " << e.what() << "\n";
        return 1;
    }

    if (state.empty()) {
        std::cerr << "Runner is not registered (empty state). Re-run 'act_runner register'.\n";
        return 1;
    }

    if (cfg.name.empty()) cfg.name = state.name;

    LOG_INFO("main", "haiku-act-runner " << cleanVersion(VERSION));
    LOG_INFO("main", "Gitea URL : " << cfg.gitea_url);
    LOG_INFO("main", "Runner    : " << cfg.name << "  UUID: " << state.uuid);
    LOG_INFO("main", "Capacity  : " << cfg.capacity);
    {
        std::string lbls;
        for (auto& l : state.labels) { if (!lbls.empty()) lbls += ' '; lbls += l; }
        LOG_INFO("main", "Labels    : " << lbls);
    }
    LOG_INFO("main", "Work dir  : " << (cfg.work_dir.empty() ? "/tmp (default)" : cfg.work_dir));

    // Signal handlers
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Install terminate handler to diagnose uncaught exceptions in threads.
    // Install an alternate signal stack and handle Haiku-internal signals.
    // See benign_signal_handler() above for the full list of handled signals.
    {
        // Alternate signal stack (64 KB) for the signal handler.
        static char sigstack_buf[65536];
        stack_t ss{};
        ss.ss_sp    = sigstack_buf;
        ss.ss_size  = sizeof(sigstack_buf);
        ss.ss_flags = 0;
        sigaltstack(&ss, nullptr);

        struct sigaction ca{};
        ca.sa_handler = benign_signal_handler;
        sigemptyset(&ca.sa_mask);
        ca.sa_flags = SA_ONSTACK;  // run on alternate stack

        // Install handlers for the Haiku-internal and recoverable signals.
        sigaction(SIGTRAP,  &ca, nullptr);  // 22 — from debug_server
        sigaction(SIGUSR2,  &ca, nullptr);  // 19 — user-defined (Haiku)
        sigaction(SIGSEGV,  &ca, nullptr);  // 11 — recoverable via siglongjmp
        sigaction(SIGBUS,   &ca, nullptr);  // 30 — recoverable via siglongjmp

        // SIGCHLD (5 on Haiku): use SIG_IGN to discard child-exit
        // notifications automatically without running our handler.
        // This avoids any race in the handler when load_image() children exit.
        struct sigaction ign{};
        ign.sa_handler = SIG_IGN;
        sigemptyset(&ign.sa_mask);
        sigaction(SIGCHLD, &ign, nullptr);  // 5 on Haiku
    }

    runner::RunnerClient client(cfg.gitea_url, cfg.insecure);
    client.setRunnerToken(state.token);
    client.setRunnerUUID(state.uuid);

    LOG_INFO("daemon", "Connecting to " << cfg.gitea_url << " ...");
    try {
        client.ping(cfg.name);
        LOG_INFO("daemon", "Connected.");
    } catch (const std::exception& e) {
        LOG_WARN("daemon", "Initial ping failed: " << e.what()
                 << " — will retry during polling");
    }

    // Declare labels to Gitea so it routes tasks to this runner.
    // Declare uses plain label names (strip ":type" suffix from stored labels).
    {
        const auto& stored_labels = state.labels;
        std::vector<std::string> declare_labels;
        for (auto& l : stored_labels) {
            auto pos = l.find(':');
            declare_labels.push_back(pos == std::string::npos ? l : l.substr(0, pos));
        }
        try {
            client.declare(declare_labels);
            LOG_INFO("daemon", "Declared labels to Gitea: "
                     << [&]{ std::string s; for(auto& l:declare_labels){s+=l+' ';}; return s; }());
        } catch (const std::exception& e) {
            LOG_WARN("daemon", "Declare failed (non-fatal): " << e.what());
        }
    }

#ifdef HAVE_MICROHTTPD
    std::unique_ptr<runner::CacheServer> cache_server;
    if (cfg.cache_enabled) {
        std::string cache_dir = cfg.cache_dir;
        if (cache_dir.empty()) {
#ifdef __HAIKU__
            BPath p;
            if (find_directory(B_USER_SETTINGS_DIRECTORY, &p) == B_OK) {
                p.Append("act_runner"); p.Append("cache");
                cache_dir = p.Path();
            } else {
                cache_dir = "/boot/home/config/settings/act_runner/cache";
            }
#else
            const char* home = getenv("HOME");
            cache_dir = (home ? std::string(home) : std::string("."))
                      + "/.cache/act_runner";
#endif
        }
        try {
            cache_server = std::make_unique<runner::CacheServer>(
                cache_dir, cfg.cache_host,
                static_cast<uint16_t>(cfg.cache_port));
            cache_server->start();
            cfg.cache_url_runtime = cache_server->baseUrl();
            // Purge stale entries on startup to prevent unbounded disk growth.
            if (cfg.cache_max_age_days > 0) {
                size_t purged = cache_server->purgeOlderThan(cfg.cache_max_age_days);
                if (purged > 0) {
                    LOG_INFO("daemon", "Cache purged " << purged
                             << " entries older than " << cfg.cache_max_age_days << " day(s)");
                }
            }
            LOG_INFO("daemon", "Local cache active at " << cfg.cache_url_runtime
                     << " (" << cache_server->entryCount() << " entries, dir="
                     << cache_dir << ")");
        } catch (const std::exception& e) {
            LOG_WARN("daemon", "Cache server failed to start: " << e.what()
                     << " — continuing without cache");
            cache_server.reset();
        }
    }
#endif

    runner::Poller poller(client, cfg, state);
    poller.start();

    LOG_INFO("daemon", "Runner started — waiting for jobs (send SIGINT/SIGTERM to stop)");

    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("daemon", "Shutdown requested — waiting for running jobs to finish...");
    poller.stop();
    poller.join();

#ifdef HAVE_MICROHTTPD
    if (cache_server) cache_server->stop();
#endif

    LOG_INFO("daemon", "Shutdown complete.");
    return 0;
}

// ─── Run subcommand ───────────────────────────────────────────────────────

static int cmdRun(const Args& args) {
    if (args.workflow_file.empty()) {
        std::cerr << "Usage: act_runner run <workflow.yml> [--event push] [--job <id>]\n"
                  << "       act_runner run --help\n";
        return 1;
    }

    // Apply log level early
    if (!args.log_level.empty()) {
        runner::Logger::instance().setLevel(args.log_level);
    }

    // Read the workflow file
    if (!std::filesystem::exists(args.workflow_file)) {
        std::cerr << "Workflow file not found: " << args.workflow_file << "\n";
        return 1;
    }

    std::string yaml_content;
    {
        std::ifstream ifs(args.workflow_file);
        if (!ifs) {
            std::cerr << "Cannot open workflow file: " << args.workflow_file << "\n";
            return 1;
        }
        yaml_content.assign(std::istreambuf_iterator<char>(ifs),
                            std::istreambuf_iterator<char>());
    }

    runner::Workflow wf;
    try {
        wf = runner::parseWorkflow(yaml_content);
    } catch (const std::exception& e) {
        std::cerr << "Workflow parse error: " << e.what() << "\n";
        return 1;
    }

    // If --job filter is specified, prune to only that job plus its transitive
    // needs so the graph stays valid.
    if (!args.job_filter.empty()) {
        if (!wf.jobs.count(args.job_filter)) {
            std::cerr << "Job '" << args.job_filter
                      << "' not found in workflow. Available jobs:";
            for (auto& [id, _] : wf.jobs) std::cerr << " " << id;
            std::cerr << "\n";
            return 1;
        }
        // Collect the target job and all its transitive needs.
        std::map<std::string, runner::Job> filtered;
        std::vector<std::string> queue = {args.job_filter};
        while (!queue.empty()) {
            std::string jid = queue.back(); queue.pop_back();
            if (filtered.count(jid) || !wf.jobs.count(jid)) continue;
            filtered[jid] = wf.jobs[jid];
            for (auto& dep : wf.jobs[jid].needs) queue.push_back(dep);
        }
        wf.jobs = filtered;
        LOG_INFO("run", "Running job '" << args.job_filter
                 << "' (and " << (wf.jobs.size()-1) << " needed job(s))");
    }

    // Resolve event payload
    std::string event_name = args.event_name.empty() ? "push" : args.event_name;
    std::string payload;
    if (!args.event_payload.empty()) {
        if (args.event_payload[0] == '@') {
            // @filename — read from file
            std::string payload_file = args.event_payload.substr(1);
            std::ifstream pf(payload_file);
            if (!pf) {
                std::cerr << "Cannot open payload file: " << payload_file << "\n";
                return 1;
            }
            payload.assign(std::istreambuf_iterator<char>(pf),
                           std::istreambuf_iterator<char>());
        } else {
            payload = args.event_payload;
        }
    }

    // Minimal config for local run
    runner::Config cfg;
    cfg.name      = getHostname();
    cfg.gitea_url = "http://localhost";  // placeholder — not used in local mode
    cfg.capacity  = 4;

    // Start the local CacheServer so that cache/artifact steps work in local
    // run mode.  Use an ephemeral port (0) on localhost.
#ifdef HAVE_MICROHTTPD
    std::unique_ptr<runner::CacheServer> cache_server;
    {
        std::string cache_dir;
#ifdef __HAIKU__
        BPath p;
        if (find_directory(B_USER_SETTINGS_DIRECTORY, &p) == B_OK) {
            p.Append("act_runner"); p.Append("cache");
            cache_dir = p.Path();
        } else {
            cache_dir = "/boot/home/config/settings/act_runner/cache";
        }
#else
        const char* home = getenv("HOME");
        cache_dir = (home ? std::string(home) : std::string("."))
                  + "/.cache/act_runner";
#endif
        try {
            cache_server = std::make_unique<runner::CacheServer>(
                cache_dir, "127.0.0.1", 0 /*ephemeral port*/);
            cache_server->start();
            cfg.cache_url_runtime = cache_server->baseUrl();
            LOG_INFO("run", "Local cache active at " << cfg.cache_url_runtime);
        } catch (const std::exception& e) {
            LOG_WARN("run", "Cache server failed to start: " << e.what()
                     << " — continuing without cache");
        }
    }
#endif

    // Set up signal handlers
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // LocalRunnerClient is a silent stub — log lines come from TaskExecutor's
    // console_cb (which calls LOG_INFO("Job", ...)) inside LogForwarder.
    runner::LocalRunnerClient local_client;

    LOG_INFO("run", "Executing workflow '" << wf.name << "' locally");
    LOG_INFO("run", "Event: " << event_name << "  Jobs: " << wf.jobs.size());

    // Warn if the declared 'on:' triggers don't include the requested event.
    // This is advisory only — allow the run to proceed (useful for manual testing).
    if (!wf.triggers.empty() && !wf.triggeredBy(event_name)) {
        std::cerr << "[WARNING] Workflow '" << wf.name << "' does not declare '"
                  << event_name << "' in its 'on:' triggers.\n"
                  << "         Declared events:";
        for (auto& [ev, _] : wf.triggers) std::cerr << " " << ev;
        std::cerr << "\n         Proceeding anyway (use --event to match a declared event).\n";
    }

    runner::WorkflowOrchestrator orchestrator(local_client, cfg);

    // Cancel via SIGINT/SIGTERM
    std::thread cancel_thread([&orchestrator]() {
        while (!g_shutdown.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        orchestrator.cancel();
    });

    // Retry loop — on Haiku the posix_spawn SIGKILLTHR race causes ~10% of
    // runs to produce a spurious FAILURE.  --retry N re-runs the whole
    // workflow up to N additional times if it fails.
    runner::OrchestratorResult result;
    const int max_attempts = 1 + args.retry_count;

    for (int attempt = 1; attempt <= max_attempts && !g_shutdown.load(); ++attempt) {
        if (attempt > 1) {
            LOG_INFO("run", "Retrying (attempt " << attempt << "/"
                     << max_attempts << ")...");
        }

        result = orchestrator.run(
            wf, yaml_content, event_name, payload,
            [attempt, max_attempts](const runner::LocalJobResult& jr) {
                if (jr.skipped) {
                    LOG_INFO("run", "  Job '" << jr.job_id << "': skipped");
                } else {
                    LOG_INFO("run", "  Job '" << jr.job_id << "': "
                             << (jr.success ? "success" : "FAILED"));
                }
            });

        if (result.success) break;
    }

    g_shutdown.store(true);  // stop cancel_thread
    cancel_thread.join();

    // Print summary
    std::cout << "\n── Workflow Run Summary ──────────────────\n";
    for (auto& jr : result.job_results) {
        std::string status = jr.skipped ? "SKIPPED"
                           : jr.success ? "success" : "FAILED";
        std::cout << "  " << jr.job_id << ": " << status << "\n";
    }
    std::cout << "─────────────────────────────────────────\n"
              << "Overall: " << (result.success ? "SUCCESS" : "FAILURE") << "\n";

    if (!result.success && args.retry_count == 0) {
        // If the user didn't specify --retry, nudge them when a step was killed.
        std::cout << "\nTip: on Haiku, step failures caused by SIGKILLTHR (signal 7)\n"
                     "     are transient. Re-run with --retry 3 to auto-recover.\n";
    }

#ifdef HAVE_MICROHTTPD
    if (cache_server) cache_server->stop();
#endif

    return result.success ? 0 : 1;
}

// ─── Unregister subcommand ────────────────────────────────────────────────

static int cmdUnregister(const Args& args) {
    std::string config_path = args.config_path.empty()
                            ? runner::defaultConfigPath()
                            : args.config_path;
    std::string state_path  = runner::defaultRunnerStatePath();

    // Load config to get gitea_url and insecure setting.
    runner::Config cfg;
    bool have_config = false;
    try {
        cfg = runner::loadConfig(config_path);
        have_config = true;
    } catch (...) {}

    // Load state to get the runner token.
    runner::RunnerState state;
    bool have_state = false;
    try {
        if (std::filesystem::exists(state_path)) {
            state = runner::loadRunnerState(state_path);
            have_state = !state.empty();
        }
    } catch (...) {}

    if (!have_state) {
        std::cerr << "No runner state found at: " << state_path << "\n"
                  << "Nothing to unregister.\n";
        return 0;   // not an error — idempotent
    }

    LOG_INFO("unregister", "Runner: " << state.name << "  UUID: " << state.uuid);

    // Best-effort: ask Gitea to remove the runner via the REST API.
    if (have_config && !cfg.gitea_url.empty() && !state.token.empty()) {
        LOG_INFO("unregister", "Contacting " << cfg.gitea_url << " to remove runner...");
        runner::RunnerClient client(cfg.gitea_url, cfg.insecure);
        client.setRunnerToken(state.token);
        client.setRunnerUUID(state.uuid);

        // The Gitea Actions API allows a runner to delete itself via:
        //   DELETE /api/v1/runners/{id}  (requires admin token — not available here)
        // or via the Connect-RPC Register mechanism (re-register with empty token
        // signals deletion in some versions).
        //
        // Since we don't have the admin token here, we simply clear the local state.
        // The runner will appear "offline" in Gitea's UI and can be removed there.
        LOG_WARN("unregister",
                 "Remote de-registration requires an admin token; "
                 "removing local state only. "
                 "Delete the runner in Gitea's web UI if needed.");
    }

    // Remove the local state file.
    try {
        if (std::filesystem::remove(state_path)) {
            LOG_INFO("unregister", "Removed runner state: " << state_path);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("unregister", "Failed to remove state file: " << e.what());
        return 1;
    }

    // Optionally clear the token from the config file.
    if (have_config) {
        cfg.runner_token = "";
        cfg.name         = "";
        try {
            runner::saveConfig(cfg, config_path);
            LOG_INFO("unregister", "Cleared token from config: " << config_path);
        } catch (...) {}
    }

    std::cout << "Runner '" << state.name << "' unregistered locally.\n"
              << "You may also remove it from Gitea's web UI:\n"
              << "  " << (have_config ? cfg.gitea_url : "<gitea_url>")
              << " → Site Administration → Actions → Runners\n";
    return 0;
}

// ─── main ─────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // curl_global_init is NOT thread-safe; call once here before any threads.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    Args args;
    try {
        args = parseArgs(argc, argv);
    } catch (const std::exception& e) {
        fprintf(stderr, "Argument error: %s\n", e.what());
        curl_global_cleanup();
        return 1;
    }

    int ret = 0;
    if (args.command == "register") {
        ret = cmdRegister(args);
    } else if (args.command == "daemon") {
        // If we're the watchdog parent, spawn a child and monitor it.
        // If we're already the watchdog child (--watchdog-child was passed),
        // fall through to cmdDaemon which runs the actual daemon loop.
        if (!args.watchdog_child) {
            ret = runWatchdog(argc, argv);
        } else {
            ret = cmdDaemon(args);
        }
    } else if (args.command == "run") {
        ret = cmdRun(args);
    } else if (args.command == "unregister") {
        ret = cmdUnregister(args);
    } else if (args.command == "version") {
        std::cout << "haiku-act-runner " << cleanVersion(VERSION) << "\n";
    } else {
        printUsage(argv[0]);
        ret = (args.command == "help" && !args.implicit_help) ? 0 : 1;
    }

    curl_global_cleanup();
    return ret;
}
