#pragma once
// RunnerClient.h — Connect-RPC client for the Gitea RunnerService
//
// Implements the 5 RPCs (Ping, Register, FetchTask, UpdateTask, UpdateLog)
// using the Connect-RPC protocol over HTTP/1.1 via libcurl.
//
// This avoids the gRPC C++ runtime dependency entirely, which is important
// for Haiku where gRPC may not be readily available as a package.
//
// Connect-RPC wire format:
//   POST <base_url>/api/actions/runner.v1.RunnerService/<MethodName>
//   POST <base_url>/api/actions/ping.v1.PingService/Ping   (for Ping only)
//   Content-Type: application/proto
//   x-runner-token: <token>
//   Body: [0x00][4-byte big-endian length][protobuf bytes]
//
// The generated proto headers (runner.pb.h) are used for encode/decode.
// If protobuf is unavailable, we provide a minimal hand-coded fallback.

#include "IRunnerClient.h"   // brings in RunnerDtos.h + IRunnerClient

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <optional>

namespace runner {

// ─── RunnerClient ─────────────────────────────────────────────────────────

class RunnerClient : public IRunnerClient {
public:
    /// @param base_url   e.g. "https://gitea.example.com"
    /// @param insecure   skip TLS verification (dev only)
    explicit RunnerClient(std::string base_url, bool insecure = false);
    ~RunnerClient();

    // Non-copyable (owns CURL handle)
    RunnerClient(const RunnerClient&)            = delete;
    RunnerClient& operator=(const RunnerClient&) = delete;

    /// Set the runner token used in x-runner-token header.
    /// Call after registration.
    void setRunnerToken(std::string token) override;

    /// Set the runner UUID (from RegisterResult::uuid). Must be called after
    /// registration — Gitea requires x-runner-uuid on every RPC call.
    void setRunnerUUID(std::string uuid);

    // ── RPCs ────────────────────────────────────────────────────────────────

    PingResult ping(const std::string& data = "") override;

    RegisterResult registerRunner(
        const std::string& reg_token,
        const std::string& name,
        const std::vector<std::string>& labels,
        const std::string& os      = "haiku",
        const std::string& arch    = "x86_64",
        const std::string& version = "0.1.0-haiku"
    ) override;

    /// Declare version + labels after registration so Gitea stores them.
    RegisterResult declare(
        const std::vector<std::string>& labels,
        const std::string& version = "0.1.0-haiku"
    ) override;

    /// Long-poll for the next available task.
    /// @param labels        runner's label list
    /// @param tasks_version version from previous FetchTaskResponse (0 on first call)
    /// @param timeout_s     HTTP timeout in seconds (should be > server long-poll window)
    FetchTaskResult fetchTask(
        const std::vector<std::string>& labels,
        int64_t tasks_version,
        int     timeout_s = 60
    ) override;

    UpdateTaskResult updateTask(
        int64_t task_id,
        int     state,          // Result enum value
        const std::vector<StepStateDto>& steps,
        int64_t started_at_s = 0,
        int64_t stopped_at_s = 0,
        const std::vector<std::pair<std::string,std::string>>& outputs = {}
    ) override;

    UpdateLogResult updateLog(
        int64_t task_id,
        int64_t index,
        const std::vector<LogRowDto>& rows,
        bool no_more = false
    ) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Encode a Connect-RPC request body (5-byte envelope + proto payload).
    // Returns raw bytes to send as HTTP POST body.
    std::string encodeRequest(const std::string& proto_bytes);

    // Decode a Connect-RPC response body, returning the inner proto bytes.
    std::string decodeResponse(const std::string& raw_body);

    // Perform a Connect-RPC call, returning the response proto bytes.
    std::string doRPC(const std::string& method,
                      const std::string& request_proto,
                      int timeout_s = 30);

    // Like doRPC but takes a full "service/Method" path (used for PingService).
    std::string doRPC_url(const std::string& service_method,
                          const std::string& request_proto,
                          int timeout_s = 30);
};

} // namespace runner
