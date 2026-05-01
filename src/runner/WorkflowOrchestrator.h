#pragma once
// WorkflowOrchestrator.h — Local multi-job workflow execution
//
// When the runner is used WITHOUT a Gitea server (e.g. `act_runner run
// workflow.yml`), this class takes a parsed Workflow, computes a wave
// schedule via JobGraph, and executes each wave in parallel up to the
// configured capacity.
//
// Under normal server-dispatched mode (Poller + TaskExecutor), Gitea's
// server handles multi-job dependency coordination — it only sends a
// FetchTask payload for a job once all its `needs:` dependencies have
// completed.  WorkflowOrchestrator is used for the local-run use-case.
//
// Execution model:
//   - Topological sort → wave schedule via JobGraph/waveSchedule()
//   - Each wave's jobs are dispatched in parallel (up to capacity)
//   - All jobs in a wave must complete before the next wave starts
//   - If any job in a wave fails (and has no continue-on-error), subsequent
//     waves are skipped (fail-fast behaviour)
//   - Job outputs (from job.outputs map) flow from earlier waves to later
//     waves via a shared results map, available as needs.<job>.outputs.*
//     in the expression context
//
// Usage:
//   WorkflowOrchestrator orch(client, cfg);
//   auto result = orch.run(workflow, event_name, event_payload_json);

#include "../workflow/WorkflowParser.h"
#include "../client/IRunnerClient.h"
#include "../config/Config.h"
#include "../client/RunnerDtos.h"

#include <string>
#include <map>
#include <vector>
#include <functional>
#include <atomic>

namespace runner {

/// Result of running a single job during local orchestration.
struct LocalJobResult {
    std::string job_id;
    bool        success     = false;
    bool        skipped     = false;
    std::map<std::string, std::string> outputs;  ///< from job.outputs map
};

/// Result of running the entire workflow.
struct OrchestratorResult {
    bool success = true;
    std::vector<LocalJobResult> job_results;

    bool allSucceeded() const {
        for (auto& r : job_results) {
            if (!r.skipped && !r.success) return false;
        }
        return true;
    }
};

/// Callback invoked after each job completes.
using JobCompleteCallback = std::function<void(const LocalJobResult&)>;

/// Runs a complete workflow locally, respecting needs: dependencies and
/// parallel wave scheduling.
class WorkflowOrchestrator {
public:
    /// @param client     runner client for UpdateTask / UpdateLog RPCs
    /// @param cfg        runner configuration (capacity, gitea_url, etc.)
    WorkflowOrchestrator(IRunnerClient& client, const Config& cfg);

    /// Run the workflow.
    ///
    /// @param workflow          parsed Workflow object
    /// @param workflow_yaml     original YAML source (passed through to TaskDto)
    /// @param event_name        e.g. "push", "pull_request", "workflow_dispatch"
    /// @param event_payload_json optional JSON event payload
    /// @param on_job_complete   optional callback after each job finishes
    ///
    /// @returns OrchestratorResult
    OrchestratorResult run(
        const Workflow&         workflow,
        const std::string&      workflow_yaml,
        const std::string&      event_name          = "push",
        const std::string&      event_payload_json  = "",
        JobCompleteCallback     on_job_complete      = nullptr
    );

    /// Signal cancellation (e.g. from SIGINT).
    void cancel() { cancelled_.store(true); }

private:
    IRunnerClient&      client_;
    const Config&       cfg_;
    std::atomic<bool>   cancelled_{false};

    /// Create a synthetic TaskDto for local job execution.
    TaskDto makeLocalTask(const std::string& workflow_yaml,
                           const std::string& job_id,
                           int64_t task_id,
                           const std::string& event_name,
                           const std::string& event_payload_json,
                           const std::map<std::string,LocalJobResult>& completed_jobs) const;
};

} // namespace runner
