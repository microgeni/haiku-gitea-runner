#pragma once
// LocalRunnerClient.h — No-op IRunnerClient for local `act_runner run` mode.
//
// When the runner executes a workflow locally (without a Gitea server),
// we still need an IRunnerClient to satisfy TaskExecutor/WorkflowOrchestrator.
// This implementation:
//   - Discards all UpdateTask / UpdateLog RPCs (no Gitea to report to).
//   - Returns benign defaults for Ping/Register/FetchTask.
//
// Log output is already handled by the LogForwarder's console_cb (set up
// in TaskExecutor), so updateLog() is intentionally a no-op here — printing
// from both places would produce duplicate lines.

#include "IRunnerClient.h"
#include "../util/Logger.h"

#include <string>
#include <vector>

namespace runner {

class LocalRunnerClient : public IRunnerClient {
public:
    LocalRunnerClient() = default;

    void setRunnerToken(std::string /*token*/) override {}

    PingResult ping(const std::string& data) override {
        return {data};
    }

    RegisterResult registerRunner(
        const std::string& /*reg_token*/,
        const std::string& name,
        const std::vector<std::string>& labels,
        const std::string& /*os*/      = "haiku",
        const std::string& /*arch*/    = "x86_64",
        const std::string& /*version*/ = "0.1.0-haiku") override
    {
        RegisterResult r;
        r.runner_token = "local";
        r.uuid         = "local";
        r.name         = name;
        r.labels       = labels;
        return r;
    }

    FetchTaskResult fetchTask(
        const std::vector<std::string>& /*labels*/,
        int64_t /*tasks_version*/,
        int     /*timeout_s*/ = 60) override
    {
        // Not used in local-run mode — return empty.
        return {};
    }

    UpdateTaskResult updateTask(
        int64_t task_id,
        int     state,
        const std::vector<StepStateDto>& /*steps*/,
        int64_t /*started_at_s*/ = 0,
        int64_t /*stopped_at_s*/ = 0) override
    {
        // In local mode we don't report to a server.
        // Just log the state change for debugging.
        static const char* stateNames[] = {"unspecified","success","failure","cancelled"};
        const char* sname = (state >= 0 && state <= 3) ? stateNames[state] : "?";
        LOG_DEBUG("LocalClient", "task " << task_id << " state → " << sname);

        UpdateTaskResult r;
        r.task_id = task_id;
        return r;
    }

    UpdateLogResult updateLog(
        int64_t /*task_id*/,
        int64_t index,
        const std::vector<LogRowDto>& rows,
        bool /*no_more*/ = false) override
    {
        // Log lines are already emitted by LogForwarder's console_cb.
        // updateLog() is the RPC delivery path — we simply ack all rows.
        UpdateLogResult r;
        r.ack_index = index + static_cast<int64_t>(rows.size());
        return r;
    }

private:
    // No mutable state needed — all operations are stateless stubs.
};

} // namespace runner
