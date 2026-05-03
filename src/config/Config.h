#pragma once
// Config.h — YAML configuration loading for haiku-act-runner
//
// Reads config.yaml (yaml-cpp) and provides a strongly-typed Config struct.
// Haiku path resolution uses find_directory() from <FindDirectory.h>.

#include <string>
#include <vector>
#include <cstdint>

namespace runner {

/// Per-label definition.
/// Format in YAML:  "name:executor"  e.g. "haiku-latest:host"
struct LabelDef {
    std::string name;      ///< label as declared in workflow files
    std::string executor;  ///< "host" (only supported on Haiku)
};

/// Top-level runner configuration (maps to config.yaml).
struct Config {
    // ── Gitea connection ──────────────────────────────────────────────────
    std::string gitea_url;     ///< e.g. "https://gitea.example.com"
    std::string runner_token;  ///< filled after registration; left empty before

    // ── Runner identity ───────────────────────────────────────────────────
    std::string name;          ///< display name shown in Gitea UI (default: hostname)
    std::string uuid;          ///< server-assigned runner UUID (written after registration)

    // ── Execution ─────────────────────────────────────────────────────────
    int         capacity = 1;  ///< max concurrent jobs (default 1 for safety)
    int         fetch_timeout  = 30;   ///< FetchTask long-poll timeout, seconds
    int         fetch_interval = 2;    ///< delay between successive FetchTask calls, seconds
    bool        insecure = false;      ///< skip TLS verification (dev/test only!)
    /// Root directory for job workspaces.  Each job gets a unique subdirectory
    /// here (e.g. <work_dir>/act_runner_<id>_<random>/).
    /// Empty = use /tmp (system default).
    std::string work_dir;

    // ── Labels ────────────────────────────────────────────────────────────
    std::vector<LabelDef> labels;      ///< executor labels

    // ── Log ───────────────────────────────────────────────────────────────
    std::string log_level = "info";    ///< "debug" | "info" | "warn" | "error"

    // ── Cache server ──────────────────────────────────────────────────────
    /// Enable the built-in actions/cache HTTP server.  When true the daemon
    /// starts a local CacheServer, exposes it to jobs via ACTIONS_CACHE_URL,
    /// and stores blobs under <cache_dir>.
    bool        cache_enabled = true;
    /// Directory for cache storage.  Empty = default (<settings>/cache).
    std::string cache_dir;
    /// Bind host for the cache server (loopback only by default).
    std::string cache_host    = "127.0.0.1";
    /// Desired listen port.  0 = ephemeral.
    int         cache_port    = 0;
    /// Purge cache entries older than this many days on startup.  0 = never.
    int         cache_max_age_days = 7;

    // ── Runtime (set by daemon, not persisted) ───────────────────────────
    /// URL of the running CacheServer, injected into job env as
    /// ACTIONS_CACHE_URL.  Empty = no cache available.
    mutable std::string cache_url_runtime;

    // ─── Helper ────────────────────────────────────────────────────────────

    /// Returns the gRPC / Connect-RPC service root URL.
    /// e.g. "https://gitea.example.com"
    std::string serviceURL() const { return gitea_url; }

    /// Returns labels as "name:executor" strings for RegisterRequest.
    std::vector<std::string> labelStrings() const;
};

// ─── Free functions ────────────────────────────────────────────────────────

/// Load configuration from a YAML file.
/// Throws std::runtime_error on parse failure.
Config loadConfig(const std::string& path);

/// Save configuration back to a YAML file (persists runner_token after reg).
/// Throws std::runtime_error on write failure.
void saveConfig(const Config& cfg, const std::string& path);

/// Return the default config file path.
/// On Haiku: B_USER_SETTINGS_DIRECTORY/act_runner/config.yaml
/// On other platforms: ~/.config/act_runner/config.yaml
std::string defaultConfigPath();

} // namespace runner
