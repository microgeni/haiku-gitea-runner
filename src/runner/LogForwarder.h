#pragma once
// LogForwarder.h — Batched log line streaming via UpdateLog RPC
//
// Background thread drains a thread-safe queue and calls UpdateLog
// in batches.  Supports the idempotent ack_index re-delivery protocol.
//
// Secret registration: pass initial secrets to the constructor so they are
// loaded BEFORE the worker thread starts.  This prevents a window where the
// worker could call maskSecrets() on the first log line before the caller has
// had a chance to call addSecret().

#include "../client/IRunnerClient.h"

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdint>
#include <ctime>

namespace runner {

/// A single log line with timestamp.
struct LogLine {
    int64_t     time_s  = 0;   ///< unix seconds
    int32_t     time_ns = 0;   ///< nanoseconds
    std::string content;        ///< line content (no trailing newline)
};

/// Callback type for log lines — used for local console output.
using LogCallback = std::function<void(const LogLine&)>;

/// Thread-safe log queue + background UpdateLog sender.
class LogForwarder {
public:
    /// @param client             RunnerClient to call UpdateLog on
    /// @param task_id            task this forwarder is associated with
    /// @param batch_size         max lines per UpdateLog call
    /// @param flush_interval_ms  flush queue at least this often (ms)
    /// @param local_cb           optional callback for console output
    /// @param initial_secrets    secret values to mask — loaded before the
    ///                           worker thread starts so no masking window exists
    LogForwarder(IRunnerClient& client,
                 int64_t        task_id,
                 size_t         batch_size        = 50,
                 int            flush_interval_ms  = 1000,
                 LogCallback    local_cb           = nullptr,
                 std::vector<std::string> initial_secrets = {});

    ~LogForwarder();

    // Non-copyable
    LogForwarder(const LogForwarder&)            = delete;
    LogForwarder& operator=(const LogForwarder&) = delete;

    /// Append a log line (thread-safe).
    void append(const std::string& line, bool is_stderr = false);

    /// Append a log line with explicit timestamp.
    void appendWithTime(int64_t unix_s, int32_t ns, const std::string& line);

    /// Wait for the background thread to flush all queued lines and send
    /// the final UpdateLog(no_more=true) call.  Call after the job finishes.
    void finish();

    /// Total lines sent so far (for UpdateTask log_length reporting).
    int64_t totalLines() const { return total_sent_.load(); }

    /// The next log-line index that will be sent (= total lines sent so far).
    /// Use this to compute per-step log_index and log_length for StepStateDto.
    int64_t currentLogIndex() const { return next_index_; }

    /// Register a secret value to be masked in all log output.
    /// Each occurrence of @p value in a log line is replaced with "***".
    /// Thread-safe.  Prefer passing secrets via the constructor for guaranteed
    /// masking from the very first log line.
    void addSecret(const std::string& value);

private:
    IRunnerClient&  client_;
    int64_t         task_id_;
    size_t          batch_size_;
    int             flush_interval_ms_;
    LogCallback     local_cb_;

    // Secret values to mask — protected by secrets_mutex_
    std::vector<std::string> secrets_;
    mutable std::mutex       secrets_mutex_;

    /// Replace every occurrence of registered secret values with "***".
    std::string maskSecrets(std::string line) const;

    std::queue<LogLine>     queue_;
    std::mutex              mutex_;
    std::condition_variable cv_;
    std::atomic<bool>       done_{false};
    std::thread             worker_;

    std::atomic<int64_t>    next_index_{0};  // next log index to send (updated by worker)
    std::atomic<int64_t>    total_sent_{0};

    void workerLoop();
    void sendBatch(std::vector<LogLine>& batch, bool no_more);
};

} // namespace runner
