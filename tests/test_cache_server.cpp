// tests/test_cache_server.cpp — Integration tests for the local cache server
//
// Starts a CacheServer on an ephemeral port and drives it with libcurl,
// covering:
//   - reserve → patch → commit → lookup (round-trip)
//   - lookup miss returns 204
//   - restore-key prefix matching
//   - double-reserve of same (key,version) returns 409
//   - blob download content matches upload
//   - path-traversal protection on /blobs/

#include "test_runner.h"
#include "../src/cache/CacheServer.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>
#include <unistd.h>    // getpid

using namespace test;
using runner::CacheServer;
namespace fs = std::filesystem;
using nlohmann::json;

// ─── libcurl helpers ──────────────────────────────────────────────────────

namespace {

struct HttpResponse {
    long        status = 0;
    std::string body;
};

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

struct ReadCtx {
    const char* p   = nullptr;
    size_t      len = 0;
};

size_t read_cb(char* dest, size_t size, size_t nmemb, void* userdata) {
    auto* r = static_cast<ReadCtx*>(userdata);
    size_t want = size * nmemb;
    size_t n    = std::min(want, r->len);
    if (n > 0) std::memcpy(dest, r->p, n);
    r->p   += n;
    r->len -= n;
    return n;
}

HttpResponse http_get(const std::string& url) {
    CURL* c = curl_easy_init();
    HttpResponse r;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &r.body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       5L);
    curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_easy_cleanup(c);
    return r;
}

HttpResponse http_post_json(const std::string& url, const std::string& body) {
    CURL* c = curl_easy_init();
    HttpResponse r;
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(c, CURLOPT_URL,         url.c_str());
    curl_easy_setopt(c, CURLOPT_POST,        1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS,  body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,  hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &r.body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       5L);
    curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return r;
}

HttpResponse http_patch(const std::string& url,
                         const std::string& body,
                         const std::string& content_range)
{
    CURL* c = curl_easy_init();
    HttpResponse r;
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/octet-stream");
    std::string range_header = "Content-Range: " + content_range;
    hdrs = curl_slist_append(hdrs, range_header.c_str());

    ReadCtx rctx{body.data(), body.size()};

    curl_easy_setopt(c, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PATCH");
    curl_easy_setopt(c, CURLOPT_UPLOAD,        1L);
    curl_easy_setopt(c, CURLOPT_READFUNCTION,  read_cb);
    curl_easy_setopt(c, CURLOPT_READDATA,      &rctx);
    curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)body.size());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &r.body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       5L);
    curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return r;
}

/// Convenience: full reserve → patch → commit round-trip.  Returns archive URL.
std::string uploadBlob(const std::string& base_url,
                        const std::string& key,
                        const std::string& version,
                        const std::string& data)
{
    json req = {{"key", key}, {"version", version}, {"cacheSize", data.size()}};
    auto reserve = http_post_json(base_url + "_apis/artifactcache/caches",
                                   req.dump());
    if (reserve.status != 200) {
        throw std::runtime_error("reserve failed: status " +
                                  std::to_string(reserve.status));
    }
    auto j = json::parse(reserve.body);
    int64_t id = j["cacheId"];

    std::string patch_url = base_url + "_apis/artifactcache/caches/" +
                             std::to_string(id);
    std::string range = "bytes 0-" +
                         std::to_string(data.size() - 1) + "/" +
                         std::to_string(data.size());
    auto patch = http_patch(patch_url, data, range);
    if (patch.status != 204) {
        throw std::runtime_error("patch failed: status " +
                                  std::to_string(patch.status));
    }

    auto commit = http_post_json(patch_url,
        std::string(R"({"size":)") + std::to_string(data.size()) + "}");
    if (commit.status != 204) {
        throw std::runtime_error("commit failed: status " +
                                  std::to_string(commit.status));
    }

    return base_url;
}

} // anonymous namespace

// ─── Fixture ──────────────────────────────────────────────────────────────

namespace {
std::string g_cache_dir;

std::string makeTempCacheDir() {
    auto tmp = fs::temp_directory_path() / ("act_runner_cache_test_" +
                                            std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    return tmp.string();
}
}

// ─── Tests ────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== CacheServer tests ===\n\n";
    curl_global_init(CURL_GLOBAL_DEFAULT);

    g_cache_dir = makeTempCacheDir();

    CacheServer server(g_cache_dir, "127.0.0.1", 0);
    server.start();
    std::string base = server.baseUrl();

    // ── lookup miss ───────────────────────────────────────────────────────

    run("lookup with no entries returns 204", [&]() {
        auto r = http_get(base + "_apis/artifactcache/cache?keys=foo&version=v1");
        ASSERT_EQ(r.status, 204L);
    });

    // ── reserve → patch → commit → lookup hit ─────────────────────────────

    run("round-trip reserve+patch+commit+lookup", [&]() {
        std::string data = "the quick brown fox jumps over the lazy dog";
        uploadBlob(base, "hello-key", "v1", data);

        auto r = http_get(base + "_apis/artifactcache/cache?keys=hello-key&version=v1");
        ASSERT_EQ(r.status, 200L);
        auto j = json::parse(r.body);
        ASSERT_EQ(j["cacheKey"].get<std::string>(), "hello-key");
        ASSERT(j.contains("archiveLocation"));

        // Fetch the blob directly — contents must match.
        std::string blob_url = j["archiveLocation"].get<std::string>();
        auto blob = http_get(blob_url);
        ASSERT_EQ(blob.status, 200L);
        ASSERT_EQ(blob.body, data);
    });

    // ── restore-key prefix match ──────────────────────────────────────────

    run("restore-key prefix matches entries uploaded under that prefix", [&]() {
        uploadBlob(base, "deps-linux-abc123", "v2", "PAYLOAD_ABC");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        uploadBlob(base, "deps-linux-def456", "v2", "PAYLOAD_DEF");

        // Primary key "deps-linux-xyz" misses, restore "deps-linux-" hits the
        // most recent matching entry.
        auto r = http_get(base + "_apis/artifactcache/cache?keys=deps-linux-xyz,deps-linux-&version=v2");
        ASSERT_EQ(r.status, 200L);
        auto j = json::parse(r.body);
        // Should be the most recent (def456)
        ASSERT_EQ(j["cacheKey"].get<std::string>(), "deps-linux-def456");
    });

    // ── version isolation ─────────────────────────────────────────────────

    run("lookup with wrong version returns 204 even if key matches", [&]() {
        uploadBlob(base, "iso-key", "v-correct", "X");

        auto r = http_get(base + "_apis/artifactcache/cache?keys=iso-key&version=v-wrong");
        ASSERT_EQ(r.status, 204L);

        auto r2 = http_get(base + "_apis/artifactcache/cache?keys=iso-key&version=v-correct");
        ASSERT_EQ(r2.status, 200L);
    });

    // ── double-reserve returns 409 ────────────────────────────────────────

    run("reserving an already-committed (key,version) returns 409", [&]() {
        uploadBlob(base, "dup-key", "v1", "first");

        json req = {{"key", "dup-key"}, {"version", "v1"}, {"cacheSize", 3}};
        auto r = http_post_json(base + "_apis/artifactcache/caches", req.dump());
        ASSERT_EQ(r.status, 409L);
    });

    // ── path traversal blocked ────────────────────────────────────────────

    run("/blobs/ path traversal rejected", [&]() {
        auto r = http_get(base + "blobs/../index.json");
        // The URL `..` in the path is typically normalised by libcurl into
        // the parent, yielding GET /index.json which our server doesn't
        // serve (404).  Either 400 or 404 is acceptable — no leak.
        ASSERT(r.status == 400L || r.status == 404L);
    });

    // ── unknown upload id ─────────────────────────────────────────────────

    run("patch with unknown upload id returns 404", [&]() {
        auto r = http_patch(base + "_apis/artifactcache/caches/999999",
                             "data", "bytes 0-3/4");
        ASSERT_EQ(r.status, 404L);
    });

    // ── entryCount tracks commits ─────────────────────────────────────────

    run("entryCount() reflects committed entries", [&]() {
        size_t before = server.entryCount();
        uploadBlob(base, "count-test-key", "vC", "abc");
        size_t after = server.entryCount();
        ASSERT_EQ(after, before + 1);
    });

    // ── invalid JSON on reserve ───────────────────────────────────────────

    run("reserve with invalid JSON returns 400", [&]() {
        auto r = http_post_json(base + "_apis/artifactcache/caches",
                                 "this is not json");
        ASSERT_EQ(r.status, 400L);
    });

    // ── index persists across restart ─────────────────────────────────────

    run("index.json persists entries across server restart", [&]() {
        uploadBlob(base, "persist-key", "vP", "PERSIST");
        size_t count_before = server.entryCount();

        // Tear down and restart on a fresh CacheServer pointing at the same dir.
        server.stop();

        CacheServer server2(g_cache_dir, "127.0.0.1", 0);
        server2.start();
        ASSERT_EQ(server2.entryCount(), count_before);

        // And the entry is still queryable.
        std::string base2 = server2.baseUrl();
        auto r = http_get(base2 + "_apis/artifactcache/cache?keys=persist-key&version=vP");
        ASSERT_EQ(r.status, 200L);

        server2.stop();
        // Restart the original for any later tests.
        server.start();
    });

    // After the restart test above, server may have a new port; refresh base URL.
    const std::string base2 = server.baseUrl();

    // ─── Artifact API (actions/upload-artifact@v4) ────────────────────────

    // Helper: PUT blob to the signed_upload_url returned by CreateArtifact
    auto artifact_put_blob = [&](const std::string& upload_url,
                                  const std::string& data) -> long {
        CURL* c = curl_easy_init();
        HttpResponse r;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Content-Type: application/octet-stream");
        ReadCtx rctx{data.data(), data.size()};
        curl_easy_setopt(c, CURLOPT_URL, upload_url.c_str());
        curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(c, CURLOPT_READFUNCTION, read_cb);
        curl_easy_setopt(c, CURLOPT_READDATA, &rctx);
        curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)data.size());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
        curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(c);
        return r.status;
    };

    run("artifact: CreateArtifact returns signed_upload_url", [&]() {
        json req = {
            {"name", "test-artifact"},
            {"workflow_run_backend_id", "42"},
            {"workflow_job_run_backend_id", "7"},
            {"version", 4},
        };
        auto r = http_post_json(
            base2 + "twirp/github.actions.results.api.v1.ArtifactService/CreateArtifact",
            req.dump());
        ASSERT_EQ(r.status, 200L);
        auto j = json::parse(r.body);
        ASSERT(j.value("ok", false) == true);
        std::string upload_url = j.value("signed_upload_url", "");
        ASSERT(!upload_url.empty());
        ASSERT(upload_url.find("/artifacts/test-artifact/blob") != std::string::npos);
    });

    run("artifact: full upload → download round-trip", [&]() {
        std::string artifact_name = "roundtrip-artifact";
        std::string payload = "artifact payload data v1.0";

        // Step 1: CreateArtifact
        json create_req = {
            {"name", artifact_name},
            {"workflow_run_backend_id", "42"},
            {"workflow_job_run_backend_id", "7"},
            {"version", 4},
        };
        auto create_r = http_post_json(
            base2 + "twirp/github.actions.results.api.v1.ArtifactService/CreateArtifact",
            create_req.dump());
        ASSERT_EQ(create_r.status, 200L);
        auto create_j = json::parse(create_r.body);
        std::string upload_url = create_j["signed_upload_url"];

        // Step 2: PUT blob
        long put_status = artifact_put_blob(upload_url, payload);
        ASSERT_EQ(put_status, 200L);

        // Step 3: FinalizeArtifact
        json fin_req = {
            {"name", artifact_name},
            {"workflow_run_backend_id", "42"},
            {"workflow_job_run_backend_id", "7"},
            {"size", std::to_string(payload.size())},
        };
        auto fin_r = http_post_json(
            base2 + "twirp/github.actions.results.api.v1.ArtifactService/FinalizeArtifact",
            fin_req.dump());
        ASSERT_EQ(fin_r.status, 200L);
        auto fin_j = json::parse(fin_r.body);
        ASSERT(fin_j.value("ok", false) == true);

        // Step 4: GetSignedArtifactURL
        json get_url_req = {
            {"name", artifact_name},
            {"workflow_run_backend_id", "42"},
            {"workflow_job_run_backend_id", "7"},
        };
        auto get_url_r = http_post_json(
            base2 + "twirp/github.actions.results.api.v1.ArtifactService/GetSignedArtifactURL",
            get_url_req.dump());
        ASSERT_EQ(get_url_r.status, 200L);
        auto get_url_j = json::parse(get_url_r.body);
        std::string download_url = get_url_j["signed_url"];
        ASSERT(!download_url.empty());

        // Step 5: GET blob
        auto get_r = http_get(download_url);
        ASSERT_EQ(get_r.status, 200L);
        ASSERT_EQ(get_r.body, payload);

        ASSERT_EQ(server.artifactCount(), 1u);
    });

    run("artifact: ListArtifacts returns committed artifacts", [&]() {
        // Upload a second artifact
        std::string name2 = "list-test-artifact";
        json create_req = {{"name", name2}, {"workflow_run_backend_id", "1"},
                           {"workflow_job_run_backend_id", "2"}, {"version", 4}};
        auto cr = http_post_json(
            base2 + "twirp/github.actions.results.api.v1.ArtifactService/CreateArtifact",
            create_req.dump());
        std::string upload_url = json::parse(cr.body)["signed_upload_url"];
        artifact_put_blob(upload_url, "data");

        json fin_req = {{"name", name2}, {"workflow_run_backend_id", "1"},
                        {"workflow_job_run_backend_id", "2"},
                        {"size", std::to_string(4)}};
        http_post_json(
            base2 + "twirp/github.actions.results.api.v1.ArtifactService/FinalizeArtifact",
            fin_req.dump());

        // List all
        auto list_r = http_post_json(
            base2 + "twirp/github.actions.results.api.v1.ArtifactService/ListArtifacts",
            "{}");
        ASSERT_EQ(list_r.status, 200L);
        auto list_j = json::parse(list_r.body);
        ASSERT(list_j.contains("artifacts"));
        ASSERT(list_j["artifacts"].size() >= 1u);

        // Filter by name
        json filter_req = {{"name_filter", {{"value", name2}}}};
        auto filtered = http_post_json(
            base2 + "twirp/github.actions.results.api.v1.ArtifactService/ListArtifacts",
            filter_req.dump());
        ASSERT_EQ(filtered.status, 200L);
        auto fj = json::parse(filtered.body);
        ASSERT_EQ(fj["artifacts"].size(), 1u);
        ASSERT_EQ(fj["artifacts"][0]["name"].get<std::string>(), name2);
    });

    run("artifact: GetSignedURL for non-existent artifact returns 404", [&]() {
        json req = {{"name", "does-not-exist"},
                    {"workflow_run_backend_id", "0"},
                    {"workflow_job_run_backend_id", "0"}};
        auto r = http_post_json(
            base2 + "twirp/github.actions.results.api.v1.ArtifactService/GetSignedArtifactURL",
            req.dump());
        ASSERT_EQ(r.status, 404L);
    });

    run("artifact: CreateArtifact with path traversal name returns 400", [&]() {
        json req = {{"name", "../evil"},
                    {"workflow_run_backend_id", "0"},
                    {"workflow_job_run_backend_id", "0"}};
        auto r = http_post_json(
            base2 + "twirp/github.actions.results.api.v1.ArtifactService/CreateArtifact",
            req.dump());
        ASSERT_EQ(r.status, 400L);
    });

    run("artifact: artifact_index.json persists across restart", [&]() {
        // Use the roundtrip-artifact already uploaded above
        size_t count_before = server.artifactCount();
        server.stop();

        CacheServer server3(g_cache_dir, "127.0.0.1", 0);
        server3.start();
        ASSERT_EQ(server3.artifactCount(), count_before);

        // The artifact should still be downloadable
        std::string base3 = server3.baseUrl();
        json req = {{"name", "roundtrip-artifact"},
                    {"workflow_run_backend_id", "42"},
                    {"workflow_job_run_backend_id", "7"}};
        auto r = http_post_json(
            base3 + "twirp/github.actions.results.api.v1.ArtifactService/GetSignedArtifactURL",
            req.dump());
        ASSERT_EQ(r.status, 200L);
        server3.stop();
        // Restart the main server for subsequent tests
        server.start();
        base = server.baseUrl();
    });

    // ── purgeOlderThan ────────────────────────────────────────────────────

    test::run("purgeOlderThan(0) purges nothing", [&]() {
        // Commit a fresh cache entry
        uploadBlob(base, "purge-test-keep", "v-purge1", "abcde");

        size_t before = server.entryCount();
        size_t purged = server.purgeOlderThan(0);  // 0 = never purge
        ASSERT_EQ(purged, 0u);
        ASSERT_EQ(server.entryCount(), before);
    });

    test::run("purgeOlderThan(365) keeps fresh entries", [&]() {
        // All entries were just created — purging with 365-day window should keep them all.
        size_t before = server.entryCount();
        size_t purged = server.purgeOlderThan(365);
        ASSERT_EQ(purged, 0u);                    // nothing is a year old
        ASSERT_EQ(server.entryCount(), before);   // count unchanged
    });

    server.stop();

    // Cleanup cache dir.
    std::error_code ec;
    fs::remove_all(g_cache_dir, ec);

    curl_global_cleanup();
    return summary();
}
