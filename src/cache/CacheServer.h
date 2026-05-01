#pragma once
// CacheServer.h — Local HTTP server for actions/cache and actions/artifact APIs
//
// Implements the GitHub Actions cache service (actions/cache@v4) and the
// artifact service (actions/upload-artifact@v4 / actions/download-artifact@v4)
// so jobs can save/restore caches and upload/download artifacts on the runner
// host without going through Gitea's server.
//
// ─── Cache API (actions/cache@v4) ─────────────────────────────────────────
// Storage layout:
//   index.json                    — map: "<key>|<version>" → CacheEntry
//   blobs/<uuid>.bin              — committed cache blobs
//   uploads/<id>/data             — in-progress uploads
//
// HTTP endpoints (cache):
//   GET  /_apis/artifactcache/cache?keys=a,b&version=v → 200/204
//   POST /_apis/artifactcache/caches                    → {"cacheId":42}
//   PATCH/_apis/artifactcache/caches/<id>               → 204
//   POST /_apis/artifactcache/caches/<id>               → 204 (commit)
//   GET  /blobs/<name>                                  → raw bytes
//
// ─── Artifact API (actions/upload-artifact@v4) ────────────────────────────
// Uses Twirp JSON protocol over HTTP, served on the same port.
// Storage layout:
//   artifact_index.json           — map: name → ArtifactEntry
//   artifacts/<name>/blob         — uploaded gzip blobs
//
// HTTP endpoints (artifact Twirp service):
//   POST /twirp/github.actions.results.api.v1.ArtifactService/CreateArtifact
//   POST /twirp/github.actions.results.api.v1.ArtifactService/FinalizeArtifact
//   POST /twirp/github.actions.results.api.v1.ArtifactService/ListArtifacts
//   POST /twirp/github.actions.results.api.v1.ArtifactService/GetSignedArtifactURL
//   PUT  /artifacts/<name>/blob    → raw upload (from CreateArtifact signed_upload_url)
//   GET  /artifacts/<name>/blob    → raw download (from GetSignedArtifactURL)
//
// ─── Environment variables the runner sets per-job ────────────────────────
//   ACTIONS_CACHE_URL     = http://127.0.0.1:<port>/   (cache API)
//   ACTIONS_RESULTS_URL   = http://127.0.0.1:<port>/   (artifact API, same port)
//   ACTIONS_RUNTIME_TOKEN = <token>                    (forwarded from Gitea task)
//
// Thread-safety: HTTP server runs on its own thread; all state guarded by mu_.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// libmicrohttpd is a hard dep of this header — included directly so that
// MHD_Result (an enum, not a tag type) can be named in the friend decl.
#include <microhttpd.h>

namespace runner {

class CacheServer {
public:
    /// @param cache_dir  root directory for blobs + index (created if missing)
    /// @param bind_host  "127.0.0.1" (default, local only) or "0.0.0.0"
    /// @param port       desired port; 0 = let OS pick an ephemeral port
    explicit CacheServer(std::string cache_dir,
                         std::string bind_host = "127.0.0.1",
                         uint16_t    port      = 0);

    ~CacheServer();

    // Non-copyable
    CacheServer(const CacheServer&)            = delete;
    CacheServer& operator=(const CacheServer&) = delete;

    /// Start the HTTP server.  Returns once listening; throws on failure.
    void start();

    /// Stop the HTTP server (idempotent).
    void stop();

    /// Returns "http://<host>:<port>/" — use this as ACTIONS_CACHE_URL and
    /// ACTIONS_RESULTS_URL.
    std::string baseUrl() const;

    /// Returns the actual listening port (useful when port=0 was requested).
    uint16_t port() const { return actual_port_; }

    /// Returns the number of cache entries currently stored.
    size_t entryCount() const;

    /// Returns the number of committed artifacts.
    size_t artifactCount() const;

    /// Purge expired entries older than max_age_days (0 = never).
    /// Returns the number of entries removed.
    size_t purgeOlderThan(int max_age_days);

private:
    // ── Cache entry ───────────────────────────────────────────────────────
    struct Entry {
        std::string key;
        std::string version;
        std::string blob_name;   // relative to <cache_dir>/blobs/
        int64_t     size          = 0;
        int64_t     created_at_ms = 0;   // unix time in milliseconds
    };

    // ── In-progress cache upload ──────────────────────────────────────────
    struct Upload {
        std::string key;
        std::string version;
        int64_t     declared_size = 0;   // from create request
        std::string path;                // <cache_dir>/uploads/<id>/data
        int64_t     received = 0;
    };

    // ── Artifact entry ────────────────────────────────────────────────────
    struct ArtifactEntry {
        std::string name;
        std::string workflow_run_id;   // echoed from CreateArtifact request
        std::string job_run_id;        // echoed from CreateArtifact request
        std::string blob_path;         // absolute path to stored blob
        int64_t     size     = 0;
        bool        finalized = false;
        int64_t     id       = 0;      // sequential artifact ID
        int64_t     created_at_ms = 0;
    };

    std::string               cache_dir_;
    std::string               bind_host_;
    uint16_t                  desired_port_;
    uint16_t                  actual_port_ = 0;

    MHD_Daemon*               daemon_ = nullptr;
    std::atomic<bool>         running_{false};

    mutable std::mutex        mu_;

    // Cache state
    std::map<std::string, Entry>   entries_;          // "<key>|<version>" → Entry
    std::map<int64_t, Upload>      uploads_;
    int64_t                        next_upload_id_ = 1;

    // Artifact state
    std::map<std::string, ArtifactEntry> artifacts_;  // name → ArtifactEntry
    int64_t                              next_artifact_id_ = 1;

    // ── Persistence ───────────────────────────────────────────────────────
    void loadIndex();
    void saveIndexLocked();         // call with mu_ held

    void loadArtifactIndex();
    void saveArtifactIndexLocked(); // call with mu_ held

    // ── Cache HTTP handlers ───────────────────────────────────────────────
    int handleCacheLookup(const std::string& keys_csv,
                          const std::string& version,
                          std::string& response_body,
                          std::string& response_type);

    int handleCacheReserve(const std::string& request_body,
                           std::string& response_body,
                           std::string& response_type);

    int handleCachePatch(int64_t upload_id,
                         const std::string& content_range,
                         const char* data, size_t data_len,
                         std::string& response_body);

    int handleCacheCommit(int64_t upload_id,
                          const std::string& request_body,
                          std::string& response_body);

    int handleBlobGet(const std::string& blob_name,
                      std::string& response_body,
                      std::string& response_type);

    // ── Artifact HTTP handlers ────────────────────────────────────────────
    // Twirp JSON service methods
    int handleArtifactCreate(const std::string& request_body,
                              std::string& response_body,
                              std::string& response_type);

    int handleArtifactFinalize(const std::string& request_body,
                                std::string& response_body,
                                std::string& response_type);

    int handleArtifactList(const std::string& request_body,
                            std::string& response_body,
                            std::string& response_type);

    int handleArtifactGetSignedUrl(const std::string& request_body,
                                    std::string& response_body,
                                    std::string& response_type);

    // Blob upload/download
    int handleArtifactBlobPut(const std::string& artifact_name,
                               const char* data, size_t data_len,
                               std::string& response_body);

    int handleArtifactBlobGet(const std::string& artifact_name,
                               std::string& response_body,
                               std::string& response_type);

    // ── Friend: MHD access handler ────────────────────────────────────────
    friend MHD_Result cache_server_access_handler(
        void* cls, MHD_Connection* connection,
        const char* url, const char* method, const char* version,
        const char* upload_data, size_t* upload_data_size,
        void** con_cls);
};

} // namespace runner
