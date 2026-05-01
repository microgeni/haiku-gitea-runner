// CacheServer.cpp — libmicrohttpd-based cache API server
#include "CacheServer.h"
#include "../util/Logger.h"

#include <microhttpd.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace fs = std::filesystem;
using nlohmann::json;

namespace runner {

// ─── Local helpers ────────────────────────────────────────────────────────

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

/// Random 32-hex-char blob filename.
static std::string randomBlobName() {
    static std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t a = dist(rng), b = dist(rng);
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(a),
                  static_cast<unsigned long long>(b));
    return std::string(buf);
}

/// Parse a comma-separated keys list with URL-decoding.
static std::vector<std::string> parseKeys(const std::string& csv) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < csv.size()) {
        size_t comma = csv.find(',', start);
        size_t end = (comma == std::string::npos) ? csv.size() : comma;
        out.emplace_back(csv.substr(start, end - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// ─── Per-connection state (accumulates PATCH/POST body chunks) ────────────

struct ConnectionState {
    std::string body;       ///< accumulated request body (for POST JSON)
    int64_t     upload_id = 0;
};

// ─── Global callback bridge ───────────────────────────────────────────────
//
// libmicrohttpd wants a C-style callback; we bind `cls` to the CacheServer
// instance and dispatch based on URL/method.

static MHD_Result send_response(MHD_Connection* conn,
                                 int status,
                                 const std::string& body,
                                 const std::string& content_type = "application/json")
{
    MHD_Response* resp = MHD_create_response_from_buffer(
        body.size(), const_cast<char*>(body.data()), MHD_RESPMEM_MUST_COPY);
    if (!content_type.empty()) {
        MHD_add_response_header(resp, "Content-Type", content_type.c_str());
    }
    MHD_Result ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return ret;
}

// ─── CacheServer: ctor/dtor ───────────────────────────────────────────────

CacheServer::CacheServer(std::string cache_dir,
                         std::string bind_host,
                         uint16_t    port)
    : cache_dir_(std::move(cache_dir))
    , bind_host_(std::move(bind_host))
    , desired_port_(port)
{
    fs::create_directories(cache_dir_);
    fs::create_directories(cache_dir_ + "/blobs");
    fs::create_directories(cache_dir_ + "/uploads");
    fs::create_directories(cache_dir_ + "/artifacts");
    loadIndex();
    loadArtifactIndex();
}

CacheServer::~CacheServer() {
    stop();
}

// ─── Index persistence ────────────────────────────────────────────────────

void CacheServer::loadIndex() {
    std::string path = cache_dir_ + "/index.json";
    std::ifstream f(path);
    if (!f.good()) return;

    try {
        json j; f >> j;
        if (!j.is_object()) return;
        for (auto& [k, v] : j.items()) {
            Entry e;
            e.key          = v.value("key", "");
            e.version      = v.value("version", "");
            e.blob_name    = v.value("blob", "");
            e.size         = v.value("size", int64_t{0});
            e.created_at_ms = v.value("created", int64_t{0});
            entries_[k] = e;
        }
        LOG_INFO("CacheServer", "Loaded " << entries_.size() << " entries from "
                 << path);
    } catch (const std::exception& e) {
        LOG_WARN("CacheServer", "Failed to parse index.json: " << e.what());
    }
}

void CacheServer::saveIndexLocked() {
    std::string path = cache_dir_ + "/index.json";
    std::string tmp  = path + ".tmp";
    {
        json j = json::object();
        for (auto& [k, e] : entries_) {
            j[k] = {
                {"key",     e.key},
                {"version", e.version},
                {"blob",    e.blob_name},
                {"size",    e.size},
                {"created", e.created_at_ms},
            };
        }
        std::ofstream f(tmp);
        f << j.dump(2);
    }
    ::rename(tmp.c_str(), path.c_str());
}

// ─── Artifact index persistence ───────────────────────────────────────────

void CacheServer::loadArtifactIndex() {
    std::string path = cache_dir_ + "/artifact_index.json";
    std::ifstream f(path);
    if (!f.good()) return;

    try {
        json j; f >> j;
        if (!j.is_object()) return;
        for (auto& [k, v] : j.items()) {
            ArtifactEntry e;
            e.name              = v.value("name", "");
            e.workflow_run_id   = v.value("workflow_run_id", "");
            e.job_run_id        = v.value("job_run_id", "");
            e.blob_path         = v.value("blob_path", "");
            e.size              = v.value("size", int64_t{0});
            e.finalized         = v.value("finalized", false);
            e.id                = v.value("id", int64_t{0});
            e.created_at_ms     = v.value("created", int64_t{0});
            artifacts_[k]       = e;
            if (e.id >= next_artifact_id_) next_artifact_id_ = e.id + 1;
        }
        LOG_INFO("CacheServer", "Loaded " << artifacts_.size()
                 << " artifacts from " << path);
    } catch (const std::exception& e) {
        LOG_WARN("CacheServer", "Failed to parse artifact_index.json: " << e.what());
    }
}

void CacheServer::saveArtifactIndexLocked() {
    std::string path = cache_dir_ + "/artifact_index.json";
    std::string tmp  = path + ".tmp";
    {
        json j = json::object();
        for (auto& [k, e] : artifacts_) {
            j[k] = {
                {"name",            e.name},
                {"workflow_run_id", e.workflow_run_id},
                {"job_run_id",      e.job_run_id},
                {"blob_path",       e.blob_path},
                {"size",            e.size},
                {"finalized",       e.finalized},
                {"id",              e.id},
                {"created",         e.created_at_ms},
            };
        }
        std::ofstream f(tmp);
        f << j.dump(2);
    }
    ::rename(tmp.c_str(), path.c_str());
}

// ─── Handlers ─────────────────────────────────────────────────────────────

int CacheServer::handleCacheLookup(const std::string& keys_csv,
                                    const std::string& version,
                                    std::string& response_body,
                                    std::string& response_type)
{
    auto keys = parseKeys(keys_csv);
    if (keys.empty()) {
        response_body = R"({"error":"missing keys"})";
        response_type = "application/json";
        return 400;
    }

    std::lock_guard<std::mutex> g(mu_);

    // Exact-match pass (primary cache lookup).
    for (auto& k : keys) {
        std::string idx = k + "|" + version;
        auto it = entries_.find(idx);
        if (it != entries_.end()) {
            json r = {
                {"cacheKey",        it->second.key},
                {"scope",           "refs/heads/main"},
                {"cacheVersion",    it->second.version},
                {"archiveLocation", baseUrl() + "blobs/" + it->second.blob_name},
            };
            response_body = r.dump();
            response_type = "application/json";
            return 200;
        }
    }

    // Prefix-match pass (restore keys — the first key is the exact key,
    // the rest are restore prefixes).  Find the newest entry whose key
    // starts with any restore-key prefix.
    const Entry* best = nullptr;
    std::string  best_idx;
    for (size_t i = 1; i < keys.size(); ++i) {
        const std::string& prefix = keys[i];
        for (auto& [idx, e] : entries_) {
            if (e.version == version
                && e.key.rfind(prefix, 0) == 0
                && (!best || e.created_at_ms > best->created_at_ms))
            {
                best = &e;
                best_idx = idx;
            }
        }
    }
    if (best) {
        json r = {
            {"cacheKey",        best->key},
            {"scope",           "refs/heads/main"},
            {"cacheVersion",    best->version},
            {"archiveLocation", baseUrl() + "blobs/" + best->blob_name},
        };
        response_body = r.dump();
        response_type = "application/json";
        return 200;
    }

    // Miss.
    return 204;
}

int CacheServer::handleCacheReserve(const std::string& request_body,
                                     std::string& response_body,
                                     std::string& response_type)
{
    json req;
    try { req = json::parse(request_body); }
    catch (...) {
        response_body = R"({"error":"invalid JSON"})";
        response_type = "application/json";
        return 400;
    }

    std::string key     = req.value("key", "");
    std::string version = req.value("version", "");
    int64_t     size    = req.value("cacheSize", int64_t{0});
    if (key.empty() || version.empty()) {
        response_body = R"({"error":"key and version required"})";
        response_type = "application/json";
        return 400;
    }

    int64_t upload_id;
    {
        std::lock_guard<std::mutex> g(mu_);
        // Already committed? Return 409 Conflict so client can skip upload.
        std::string idx = key + "|" + version;
        if (entries_.count(idx)) {
            response_body = R"({"error":"already exists"})";
            response_type = "application/json";
            return 409;
        }

        upload_id = next_upload_id_++;
        Upload u;
        u.key           = key;
        u.version       = version;
        u.declared_size = size;
        u.path          = cache_dir_ + "/uploads/" + std::to_string(upload_id);
        fs::create_directories(u.path);
        u.path += "/data";
        uploads_[upload_id] = u;

        // Touch the file (truncate to 0).
        std::ofstream f(u.path, std::ios::binary | std::ios::trunc);
    }

    json r = {{"cacheId", upload_id}};
    response_body = r.dump();
    response_type = "application/json";
    return 200;
}

int CacheServer::handleCachePatch(int64_t upload_id,
                                   const std::string& content_range,
                                   const char* data, size_t data_len,
                                   std::string& response_body)
{
    // content_range example: "bytes 0-1048575/5000000"
    int64_t start = 0, end = 0, total = 0;
    if (!content_range.empty()) {
        std::sscanf(content_range.c_str(), "bytes %lld-%lld/%lld",
                    reinterpret_cast<long long*>(&start),
                    reinterpret_cast<long long*>(&end),
                    reinterpret_cast<long long*>(&total));
    }
    (void)total;

    std::string path;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = uploads_.find(upload_id);
        if (it == uploads_.end()) {
            response_body = R"({"error":"unknown upload id"})";
            return 404;
        }
        path = it->second.path;
    }

    // Write at offset `start`.
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT, 0600);
    if (fd < 0) {
        response_body = R"({"error":"cannot open upload file"})";
        return 500;
    }
    if (start > 0) ::lseek(fd, start, SEEK_SET);
    const char* p = data;
    size_t remaining = data_len;
    while (remaining > 0) {
        ssize_t n = ::write(fd, p, remaining);
        if (n <= 0) { ::close(fd); response_body = R"({"error":"write failed"})"; return 500; }
        p         += n;
        remaining -= static_cast<size_t>(n);
    }
    ::close(fd);

    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = uploads_.find(upload_id);
        if (it != uploads_.end()) {
            int64_t written_end = (end >= start) ? (end + 1)
                                                  : static_cast<int64_t>(start + data_len);
            if (written_end > it->second.received) it->second.received = written_end;
        }
    }
    response_body = "";
    return 204;
}

int CacheServer::handleCacheCommit(int64_t upload_id,
                                    const std::string& /*request_body*/,
                                    std::string& response_body)
{
    Upload u;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = uploads_.find(upload_id);
        if (it == uploads_.end()) {
            response_body = R"({"error":"unknown upload id"})";
            return 404;
        }
        u = it->second;
    }

    // Move the uploaded file into blobs/ with a random name.
    std::string blob_name = randomBlobName() + ".bin";
    std::string dest      = cache_dir_ + "/blobs/" + blob_name;

    std::error_code ec;
    fs::rename(u.path, dest, ec);
    if (ec) {
        // rename across filesystems → copy+remove
        fs::copy_file(u.path, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            response_body = R"({"error":"blob commit failed"})";
            return 500;
        }
        fs::remove(u.path);
    }

    int64_t size = 0;
    try { size = fs::file_size(dest); } catch (...) {}

    {
        std::lock_guard<std::mutex> g(mu_);
        Entry e;
        e.key          = u.key;
        e.version      = u.version;
        e.blob_name    = blob_name;
        e.size         = size;
        e.created_at_ms = now_ms();
        entries_[u.key + "|" + u.version] = e;
        uploads_.erase(upload_id);
        saveIndexLocked();
    }

    // Cleanup the upload dir.
    try { fs::remove_all(cache_dir_ + "/uploads/" + std::to_string(upload_id)); }
    catch (...) {}

    response_body = "";
    return 204;
}

int CacheServer::handleBlobGet(const std::string& blob_name,
                                std::string& response_body,
                                std::string& response_type)
{
    // Reject path traversal.
    if (blob_name.find('/')  != std::string::npos ||
        blob_name.find("..") != std::string::npos)
    {
        response_body = R"({"error":"invalid blob name"})";
        response_type = "application/json";
        return 400;
    }
    std::string path = cache_dir_ + "/blobs/" + blob_name;
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) {
        response_body = R"({"error":"not found"})";
        response_type = "application/json";
        return 404;
    }
    std::ostringstream ss; ss << f.rdbuf();
    response_body = ss.str();
    response_type = "application/octet-stream";
    return 200;
}

// ─── Artifact handlers ────────────────────────────────────────────────────
//
// Implements the Twirp JSON service:
//   POST /twirp/github.actions.results.api.v1.ArtifactService/<Method>
// and the blob storage endpoints:
//   PUT  /artifacts/<name>/blob  (upload)
//   GET  /artifacts/<name>/blob  (download)

int CacheServer::handleArtifactCreate(const std::string& request_body,
                                       std::string& response_body,
                                       std::string& response_type)
{
    json req;
    try { req = json::parse(request_body); }
    catch (...) {
        response_body = R"({"code":"invalid_argument","msg":"invalid JSON"})";
        response_type = "application/json";
        return 400;
    }

    std::string name           = req.value("name", "");
    std::string workflow_run_id = req.value("workflow_run_backend_id", "");
    std::string job_run_id     = req.value("workflow_job_run_backend_id", "");

    if (name.empty()) {
        response_body = R"({"code":"invalid_argument","msg":"name required"})";
        response_type = "application/json";
        return 400;
    }
    // Sanitise name to prevent directory traversal
    if (name.find('/') != std::string::npos || name.find("..") != std::string::npos) {
        response_body = R"({"code":"invalid_argument","msg":"invalid artifact name"})";
        response_type = "application/json";
        return 400;
    }

    std::string blob_path = cache_dir_ + "/artifacts/" + name + ".bin";
    int64_t artifact_id;

    {
        std::lock_guard<std::mutex> g(mu_);
        artifact_id = next_artifact_id_++;
        ArtifactEntry e;
        e.name              = name;
        e.workflow_run_id   = workflow_run_id;
        e.job_run_id        = job_run_id;
        e.blob_path         = blob_path;
        e.size              = 0;
        e.finalized         = false;
        e.id                = artifact_id;
        e.created_at_ms     = now_ms();
        artifacts_[name]    = e;
        saveArtifactIndexLocked();
    }

    LOG_INFO("CacheServer", "Artifact create: name='" << name
             << "' id=" << artifact_id);

    // The signed_upload_url is the URL the toolkit PUTs the blob to.
    std::string upload_url = baseUrl() + "artifacts/" + name + "/blob";

    json r = {
        {"ok",                true},
        {"signed_upload_url", upload_url},
    };
    response_body = r.dump();
    response_type = "application/json";
    return 200;
}

int CacheServer::handleArtifactFinalize(const std::string& request_body,
                                         std::string& response_body,
                                         std::string& response_type)
{
    json req;
    try { req = json::parse(request_body); }
    catch (...) {
        response_body = R"({"code":"invalid_argument","msg":"invalid JSON"})";
        response_type = "application/json";
        return 400;
    }

    std::string name = req.value("name", "");
    // size is a string in the Twirp JSON encoding (int64 proto → string)
    int64_t declared_size = 0;
    if (req.contains("size")) {
        auto& sv = req["size"];
        if (sv.is_string()) declared_size = std::stoll(sv.get<std::string>());
        else if (sv.is_number()) declared_size = sv.get<int64_t>();
    }

    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = artifacts_.find(name);
        if (it == artifacts_.end()) {
            response_body = R"({"code":"not_found","msg":"artifact not found"})";
            response_type = "application/json";
            return 404;
        }

        // Measure actual blob size
        int64_t actual_size = 0;
        try { actual_size = fs::file_size(it->second.blob_path); } catch (...) {}

        it->second.size       = actual_size > 0 ? actual_size : declared_size;
        it->second.finalized  = true;
        saveArtifactIndexLocked();
    }

    LOG_INFO("CacheServer", "Artifact finalize: name='" << name
             << "' size=" << declared_size);

    std::string artifact_id_str;
    {
        std::lock_guard<std::mutex> g(mu_);
        artifact_id_str = std::to_string(artifacts_.at(name).id);
    }

    json r = {
        {"ok",          true},
        {"artifact_id", artifact_id_str},  // string, not number (Twirp int64)
    };
    response_body = r.dump();
    response_type = "application/json";
    return 200;
}

int CacheServer::handleArtifactList(const std::string& request_body,
                                     std::string& response_body,
                                     std::string& response_type)
{
    json req;
    try { req = json::parse(request_body); }
    catch (...) { req = json::object(); }

    // Optional name filter
    std::string name_filter;
    if (req.contains("name_filter") && req["name_filter"].is_object()) {
        name_filter = req["name_filter"].value("value", "");
    }

    json artifacts_arr = json::array();
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& [k, e] : artifacts_) {
            if (!e.finalized) continue;
            if (!name_filter.empty() && e.name != name_filter) continue;

            artifacts_arr.push_back({
                {"workflow_run_backend_id",     e.workflow_run_id},
                {"workflow_job_run_backend_id", e.job_run_id},
                {"database_id",                std::to_string(e.id)},
                {"name",                        e.name},
                {"size",                        std::to_string(e.size)},
            });
        }
    }

    json r = {{"artifacts", artifacts_arr}};
    response_body = r.dump();
    response_type = "application/json";
    return 200;
}

int CacheServer::handleArtifactGetSignedUrl(const std::string& request_body,
                                             std::string& response_body,
                                             std::string& response_type)
{
    json req;
    try { req = json::parse(request_body); }
    catch (...) {
        response_body = R"({"code":"invalid_argument","msg":"invalid JSON"})";
        response_type = "application/json";
        return 400;
    }

    std::string name = req.value("name", "");
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = artifacts_.find(name);
        if (it == artifacts_.end() || !it->second.finalized) {
            response_body = R"({"code":"not_found","msg":"artifact not found"})";
            response_type = "application/json";
            return 404;
        }
    }

    std::string download_url = baseUrl() + "artifacts/" + name + "/blob";

    json r = {{"signed_url", download_url}};
    response_body = r.dump();
    response_type = "application/json";
    return 200;
}

int CacheServer::handleArtifactBlobPut(const std::string& artifact_name,
                                        const char* data, size_t data_len,
                                        std::string& response_body)
{
    std::string blob_path;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = artifacts_.find(artifact_name);
        if (it == artifacts_.end()) {
            response_body = R"({"error":"artifact not created"})";
            return 404;
        }
        blob_path = it->second.blob_path;
    }

    // Write blob (overwrite any existing)
    std::ofstream f(blob_path, std::ios::binary | std::ios::trunc);
    if (!f.good()) {
        response_body = R"({"error":"cannot write blob"})";
        return 500;
    }
    f.write(data, static_cast<std::streamsize>(data_len));
    f.close();

    LOG_INFO("CacheServer", "Artifact blob uploaded: name='" << artifact_name
             << "' bytes=" << data_len);

    response_body = "";
    return 200;
}

int CacheServer::handleArtifactBlobGet(const std::string& artifact_name,
                                        std::string& response_body,
                                        std::string& response_type)
{
    std::string blob_path;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = artifacts_.find(artifact_name);
        if (it == artifacts_.end() || !it->second.finalized) {
            response_body = R"({"error":"artifact not found"})";
            response_type = "application/json";
            return 404;
        }
        blob_path = it->second.blob_path;
    }

    std::ifstream f(blob_path, std::ios::binary);
    if (!f.good()) {
        response_body = R"({"error":"blob not readable"})";
        response_type = "application/json";
        return 404;
    }
    std::ostringstream ss; ss << f.rdbuf();
    response_body = ss.str();
    response_type = "application/octet-stream";
    return 200;
}

// ─── MHD access handler (C callback) ──────────────────────────────────────

MHD_Result cache_server_access_handler(
    void* cls, MHD_Connection* conn,
    const char* url, const char* method, const char* /*version*/,
    const char* upload_data, size_t* upload_data_size,
    void** con_cls)
{
    auto* self = static_cast<CacheServer*>(cls);

    // First invocation: allocate per-connection state.  MHD calls this
    // callback multiple times for the same request (headers, then body
    // chunks, then finalise).  We must return MHD_YES without queueing
    // a response until all body bytes are consumed.
    if (*con_cls == nullptr) {
        auto* state = new ConnectionState();
        *con_cls = state;
        return MHD_YES;
    }
    auto* state = static_cast<ConnectionState*>(*con_cls);

    std::string smethod = method ? method : "";
    std::string surl    = url ? url : "";

    // For requests with body: accumulate chunks until MHD signals "no more".
    if ((smethod == "POST" || smethod == "PATCH" || smethod == "PUT")
        && *upload_data_size != 0)
    {
        state->body.append(upload_data, *upload_data_size);
        *upload_data_size = 0;   // consumed
        return MHD_YES;
    }

    // Route.
    std::string body;
    std::string ctype = "application/json";
    int status = 404;

    try {
        // ── GET /_apis/artifactcache/cache?keys=...&version=... ───────────
        if (smethod == "GET" && surl == "/_apis/artifactcache/cache") {
            const char* keys    = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "keys");
            const char* version = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "version");
            status = self->handleCacheLookup(keys ? keys : "",
                                             version ? version : "",
                                             body, ctype);
        }
        // ── POST /_apis/artifactcache/caches — reserve ────────────────────
        else if (smethod == "POST" && surl == "/_apis/artifactcache/caches") {
            status = self->handleCacheReserve(state->body, body, ctype);
        }
        // ── PATCH /_apis/artifactcache/caches/<id> — upload chunk ─────────
        else if (smethod == "PATCH" && surl.rfind("/_apis/artifactcache/caches/", 0) == 0) {
            int64_t id = std::strtoll(surl.c_str() + 28, nullptr, 10);
            const char* range = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Content-Range");
            status = self->handleCachePatch(id, range ? range : "",
                                             state->body.data(),
                                             state->body.size(),
                                             body);
            ctype = body.empty() ? "" : "application/json";
        }
        // ── POST /_apis/artifactcache/caches/<id> — commit ────────────────
        else if (smethod == "POST" && surl.rfind("/_apis/artifactcache/caches/", 0) == 0) {
            int64_t id = std::strtoll(surl.c_str() + 28, nullptr, 10);
            status = self->handleCacheCommit(id, state->body, body);
            ctype = body.empty() ? "" : "application/json";
        }
        // ── GET /blobs/<name> — download ──────────────────────────────────
        else if (smethod == "GET" && surl.rfind("/blobs/", 0) == 0) {
            status = self->handleBlobGet(surl.substr(7), body, ctype);
        }
        // ── Artifact Twirp service methods ────────────────────────────────
        // All are POST with JSON body, JSON response.
        else if (smethod == "POST" && surl == "/twirp/github.actions.results.api.v1.ArtifactService/CreateArtifact") {
            status = self->handleArtifactCreate(state->body, body, ctype);
        }
        else if (smethod == "POST" && surl == "/twirp/github.actions.results.api.v1.ArtifactService/FinalizeArtifact") {
            status = self->handleArtifactFinalize(state->body, body, ctype);
        }
        else if (smethod == "POST" && surl == "/twirp/github.actions.results.api.v1.ArtifactService/ListArtifacts") {
            status = self->handleArtifactList(state->body, body, ctype);
        }
        else if (smethod == "POST" && surl == "/twirp/github.actions.results.api.v1.ArtifactService/GetSignedArtifactURL") {
            status = self->handleArtifactGetSignedUrl(state->body, body, ctype);
        }
        // ── Artifact blob upload: PUT /artifacts/<name>/blob ──────────────
        else if (smethod == "PUT" && surl.rfind("/artifacts/", 0) == 0
                 && surl.size() > 12
                 && surl.substr(surl.size() - 5) == "/blob")
        {
            // Extract artifact name from URL: /artifacts/<name>/blob
            std::string name = surl.substr(11, surl.size() - 11 - 5);
            status = self->handleArtifactBlobPut(
                name,
                state->body.data(), state->body.size(),
                body);
            ctype = body.empty() ? "" : "application/json";
        }
        // ── Artifact blob download: GET /artifacts/<name>/blob ────────────
        else if (smethod == "GET" && surl.rfind("/artifacts/", 0) == 0
                 && surl.size() > 12
                 && surl.substr(surl.size() - 5) == "/blob")
        {
            std::string name = surl.substr(11, surl.size() - 11 - 5);
            status = self->handleArtifactBlobGet(name, body, ctype);
        }
        // ── Unknown route ─────────────────────────────────────────────────
        else {
            body  = R"({"error":"not found"})";
            ctype = "application/json";
            status = 404;
        }
    } catch (const std::exception& e) {
        body  = std::string(R"({"error":")") + e.what() + R"("})";
        ctype = "application/json";
        status = 500;
        LOG_WARN("CacheServer", "handler threw: " << e.what());
    }

    // Cleanup per-connection state.
    delete state;
    *con_cls = nullptr;

    // 204 No Content: send empty response without Content-Type.
    if (status == 204) {
        MHD_Response* resp = MHD_create_response_from_buffer(
            0, const_cast<char*>(""), MHD_RESPMEM_PERSISTENT);
        MHD_Result ret = MHD_queue_response(conn, 204, resp);
        MHD_destroy_response(resp);
        return ret;
    }
    return send_response(conn, status, body, ctype);
}

// ─── start/stop ───────────────────────────────────────────────────────────

void CacheServer::start() {
    if (running_.load()) return;

    // We bind only to the loopback by default.  libmicrohttpd's sockaddr
    // option would allow explicit host; we keep it simple and rely on
    // MHD_start_daemon with MHD_OPTION_SOCK_ADDR if we ever need remote
    // bind.  For now, port-only.
    unsigned int flags = MHD_USE_INTERNAL_POLLING_THREAD;

    daemon_ = MHD_start_daemon(
        flags,
        desired_port_,
        nullptr, nullptr,
        &cache_server_access_handler, this,
        MHD_OPTION_END);
    if (!daemon_) {
        throw std::runtime_error("MHD_start_daemon failed — port in use?");
    }

    // Discover actual listen port (matters when desired_port_=0).
    const union MHD_DaemonInfo* info =
        MHD_get_daemon_info(daemon_, MHD_DAEMON_INFO_BIND_PORT);
    actual_port_ = info ? info->port : desired_port_;

    running_.store(true);
    LOG_INFO("CacheServer", "Listening on " << baseUrl()
             << " (cache_dir=" << cache_dir_ << ")");
}

void CacheServer::stop() {
    if (!running_.exchange(false)) return;
    if (daemon_) {
        MHD_stop_daemon(daemon_);
        daemon_ = nullptr;
    }
    LOG_INFO("CacheServer", "Stopped");
}

// ─── Introspection ────────────────────────────────────────────────────────

std::string CacheServer::baseUrl() const {
    std::ostringstream ss;
    ss << "http://" << bind_host_ << ":" << actual_port_ << "/";
    return ss.str();
}

size_t CacheServer::entryCount() const {
    std::lock_guard<std::mutex> g(mu_);
    return entries_.size();
}

size_t CacheServer::artifactCount() const {
    std::lock_guard<std::mutex> g(mu_);
    size_t n = 0;
    for (auto& [k, e] : artifacts_) {
        if (e.finalized) ++n;
    }
    return n;
}

size_t CacheServer::purgeOlderThan(int max_age_days) {
    if (max_age_days <= 0) return 0;
    int64_t cutoff_ms = now_ms() - static_cast<int64_t>(max_age_days) * 86400LL * 1000LL;

    size_t removed = 0;
    std::lock_guard<std::mutex> g(mu_);
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        if (it->second.created_at_ms < cutoff_ms) {
            std::error_code ec;
            fs::remove(cache_dir_ + "/blobs/" + it->second.blob_name, ec);
            it = entries_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed > 0) saveIndexLocked();
    return removed;
}

} // namespace runner
