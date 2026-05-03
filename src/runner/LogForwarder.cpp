// LogForwarder.cpp — Background log streaming implementation
#include "LogForwarder.h"
#include "../util/Logger.h"

#include <chrono>
#ifdef __HAIKU__
#include <signal.h>
#include <pthread.h>
#endif

namespace runner {

LogForwarder::LogForwarder(IRunnerClient& client,
                             int64_t        task_id,
                             size_t         batch_size,
                             int            flush_interval_ms,
                             LogCallback    local_cb,
                             std::vector<std::string> initial_secrets)
    : client_(client)
    , task_id_(task_id)
    , batch_size_(batch_size)
    , flush_interval_ms_(flush_interval_ms)
    , local_cb_(std::move(local_cb))
    , secrets_(std::move(initial_secrets))   // loaded BEFORE worker starts
{
    worker_ = std::thread([this]() {
#ifdef __HAIKU__
        sigset_t block; sigemptyset(&block);
        sigaddset(&block, SIGKILLTHR);
        pthread_sigmask(SIG_BLOCK, &block, nullptr);
#endif
        workerLoop();
    });
}

LogForwarder::~LogForwarder() {
    finish();
}

void LogForwarder::addSecret(const std::string& value) {
    if (value.empty()) return;
    std::lock_guard<std::mutex> lk(secrets_mutex_);
    // Avoid duplicates
    for (auto& s : secrets_) {
        if (s == value) return;
    }
    secrets_.push_back(value);
}

std::string LogForwarder::maskSecrets(std::string line) const {
    std::lock_guard<std::mutex> lk(secrets_mutex_);
    for (const auto& secret : secrets_) {
        size_t pos = 0;
        while ((pos = line.find(secret, pos)) != std::string::npos) {
            line.replace(pos, secret.size(), "***");
            pos += 3;  // length of "***"
        }
    }
    return line;
}

void LogForwarder::append(const std::string& line, bool /*is_stderr*/) {
    auto now = std::chrono::system_clock::now();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    now.time_since_epoch()).count();
    auto ns   = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now.time_since_epoch()).count() % 1'000'000'000LL;

    appendWithTime(secs, static_cast<int32_t>(ns), line);
}

void LogForwarder::appendWithTime(int64_t unix_s, int32_t ns, const std::string& line) {
    // Mask secret values before storing or forwarding the line.
    std::string masked = maskSecrets(line);

    LogLine ll{unix_s, ns, masked};

    if (local_cb_) local_cb_(ll);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(ll));
    }
    cv_.notify_one();
}

void LogForwarder::finish() {
    if (done_.exchange(true)) return;  // already finished
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void LogForwarder::workerLoop() {
    while (true) {
        std::vector<LogLine> batch;
        batch.reserve(batch_size_);

        {
            std::unique_lock<std::mutex> lock(mutex_);
            auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(flush_interval_ms_);

            // Wait for either: batch full, flush interval, or done
            cv_.wait_until(lock, deadline, [this]() {
                return !queue_.empty() || done_.load();
            });

            // Drain up to batch_size_ items
            while (!queue_.empty() && batch.size() < batch_size_) {
                batch.push_back(std::move(queue_.front()));
                queue_.pop();
            }
        }

        bool is_last = done_.load();

        if (!batch.empty() || is_last) {
            sendBatch(batch, is_last);
        }

        if (is_last) break;
    }
}

void LogForwarder::sendBatch(std::vector<LogLine>& batch, bool no_more) {
    if (batch.empty() && !no_more) return;

    std::vector<LogRowDto> rows;
    rows.reserve(batch.size());
    for (auto& ll : batch) {
        rows.push_back({ll.time_s, ll.time_ns, ll.content});
    }

    int64_t index = next_index_.load();
    int retries = 3;

    while (retries-- > 0) {
        try {
            auto result = client_.updateLog(task_id_, index, rows, no_more);
            int64_t ack = result.ack_index;

            if (ack < index + static_cast<int64_t>(rows.size())) {
                // Server missed some rows — resend from ack_index
                int64_t skip = ack - index;
                if (skip > 0 && skip <= static_cast<int64_t>(rows.size())) {
                    rows.erase(rows.begin(), rows.begin() + skip);
                }
                index = ack;
                continue;  // retry
            }

            next_index_.store(index + static_cast<int64_t>(rows.size()));
            total_sent_.fetch_add(static_cast<int64_t>(batch.size()),
                                  std::memory_order_relaxed);
            return;
        } catch (const std::exception& e) {
            LOG_WARN("LogForwarder", "UpdateLog failed: " << e.what());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    // On repeated failure, advance index anyway to avoid stalling
    next_index_.fetch_add(static_cast<int64_t>(rows.size()));
}

} // namespace runner
