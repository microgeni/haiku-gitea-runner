#pragma once
// tests/MockRunnerClient.h — In-memory IRunnerClient for integration tests
//
// Records all RPC calls so tests can inspect them.  No network I/O.
//
// Typical usage:
//   MockRunnerClient mock;
//   mock.next_task_ = makeTask(...);        // returned by fetchTask()
//   TaskExecutor exec(mock, task, cfg);
//   bool ok = exec.execute();
//   ASSERT_EQ(mock.update_task_calls_.back().state, 1 /*SUCCESS*/);
//   ASSERT(!mock.log_lines_.empty());

#include "../src/client/IRunnerClient.h"

#include <mutex>
#include <vector>
#include <string>
#include <optional>

namespace test {

struct UpdateTaskCall {
    int64_t task_id = 0;
    int     state   = 0;   // Result enum (0=unspecified, 1=success, 2=failure, 3=cancelled)
    std::vector<runner::StepStateDto> steps;
    int64_t started_at_s = 0;
    int64_t stopped_at_s = 0;
};

struct UpdateLogCall {
    int64_t task_id = 0;
    int64_t index   = 0;
    std::vector<runner::LogRowDto> rows;
    bool    no_more = false;
};

class MockRunnerClient : public runner::IRunnerClient {
public:
    // ── Inputs (set these before the call) ────────────────────────────────

    /// If set, fetchTask() returns this task once (then returns empty).
    std::optional<runner::TaskDto> next_task_;

    /// Value returned from fetchTask().tasks_version.
    int64_t fetch_tasks_version_ = 0;

    /// If non-empty, updateLog() reports this ack_index each call.
    std::optional<int64_t> log_ack_override_;

    // ── Recorded outputs (inspect after the call) ─────────────────────────

    mutable std::mutex mu_;
    std::condition_variable no_more_cv_;   // notified when got_no_more_ is set

    std::vector<UpdateTaskCall>  update_task_calls_;
    std::vector<UpdateLogCall>   update_log_calls_;
    std::vector<runner::LogRowDto> log_lines_;   // flat concat of all rows
    int  ping_count_     = 0;
    int  register_count_ = 0;
    int  fetch_count_    = 0;
    bool got_no_more_    = false;

    // ── IRunnerClient ─────────────────────────────────────────────────────

    void setRunnerToken(std::string /*token*/) override {}

    runner::PingResult ping(const std::string& data = "") override {
        std::lock_guard<std::mutex> g(mu_);
        ++ping_count_;
        return {data};
    }

    runner::RegisterResult registerRunner(
        const std::string& /*reg_token*/,
        const std::string& name,
        const std::vector<std::string>& labels,
        const std::string& /*os*/      = "haiku",
        const std::string& /*arch*/    = "x86_64",
        const std::string& /*version*/ = "0.1.0-haiku") override
    {
        std::lock_guard<std::mutex> g(mu_);
        ++register_count_;
        runner::RegisterResult r;
        r.runner_token = "mock-runner-token";
        r.uuid         = "mock-uuid";
        r.name         = name;
        r.labels       = labels;
        return r;
    }

    runner::RegisterResult declare(
        const std::vector<std::string>& labels,
        const std::string& /*version*/ = "0.1.0-haiku") override
    {
        runner::RegisterResult r;
        r.runner_token = "mock-runner-token";
        r.labels       = labels;
        return r;
    }

    runner::FetchTaskResult fetchTask(
        const std::vector<std::string>& /*labels*/,
        int64_t /*tasks_version*/,
        int     /*timeout_s*/ = 60) override
    {
        std::lock_guard<std::mutex> g(mu_);
        ++fetch_count_;
        runner::FetchTaskResult r;
        r.tasks_version = fetch_tasks_version_;
        if (next_task_) {
            r.task = *next_task_;
            next_task_.reset();   // deliver once
        }
        return r;
    }

    runner::UpdateTaskResult updateTask(
        int64_t task_id,
        int     state,
        const std::vector<runner::StepStateDto>& steps,
        int64_t started_at_s = 0,
        int64_t stopped_at_s = 0,
        const std::vector<std::pair<std::string,std::string>>& /*outputs*/ = {}) override
    {
        std::lock_guard<std::mutex> g(mu_);
        update_task_calls_.push_back({task_id, state, steps,
                                       started_at_s, stopped_at_s});
        runner::UpdateTaskResult r;
        r.task_id = task_id;
        r.state   = "";
        return r;
    }

    runner::UpdateLogResult updateLog(
        int64_t task_id,
        int64_t index,
        const std::vector<runner::LogRowDto>& rows,
        bool no_more = false) override
    {
        {
            std::lock_guard<std::mutex> g(mu_);
            update_log_calls_.push_back({task_id, index, rows, no_more});
            if (no_more) got_no_more_ = true;
            // Always fully acknowledge (unless override)
            for (auto& r : rows) log_lines_.push_back(r);
        }
        // Notify outside the lock so waiters can immediately re-acquire.
        if (no_more) no_more_cv_.notify_all();

        std::lock_guard<std::mutex> g(mu_);
        runner::UpdateLogResult r;
        r.ack_index = log_ack_override_.has_value()
                    ? *log_ack_override_
                    : index + static_cast<int64_t>(rows.size());
        return r;
    }

    // ── Convenience inspectors ────────────────────────────────────────────

    /// Block until the final UpdateLog(no_more=true) has been received,
    /// or until timeout_ms elapses.  Use this before calling allLogs() to
    /// guarantee all log lines have been written.
    bool waitForNoMore(int timeout_ms = 10000) {
        std::unique_lock<std::mutex> lk(mu_);
        return no_more_cv_.wait_for(
            lk,
            std::chrono::milliseconds(timeout_ms),
            [this]() { return got_no_more_; });
    }

    /// Return the concatenation of all log line contents, newline-joined.
    /// Must hold mu_ while iterating log_lines_ — updateLog() writes under mu_.
    std::string allLogs() const {
        std::lock_guard<std::mutex> g(mu_);
        std::string out;
        for (auto& r : log_lines_) {
            out += r.content;
            out += '\n';
        }
        return out;
    }

    /// Final UpdateTask state value (0 if none), e.g. 1 = success, 2 = failure.
    int finalTaskState() const {
        std::lock_guard<std::mutex> g(mu_);
        if (update_task_calls_.empty()) return 0;
        return update_task_calls_.back().state;
    }
};

} // namespace test
