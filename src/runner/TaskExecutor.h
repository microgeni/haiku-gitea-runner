#pragma once
// TaskExecutor.h — Orchestrates execution of a single Gitea Actions task
//
// Sequence:
//  1. Parse workflow YAML from task payload
//  2. Find the job to execute (by machine name)
//  3. UpdateTask(running) → server
//  4. For each step:
//     a. Evaluate 'if:' condition
//     b. Run step via StepRunner
//     c. Collect outputs, update ContextBuilder
//     d. Send step UpdateTask with StepState
//     e. If step had a JS action with post: script, enqueue for later
//  5. Run all enqueued post: scripts in reverse order
//  6. UpdateLog(no_more=true)
//  7. UpdateTask(success/failure) → server

#include "../client/IRunnerClient.h"
#include "../config/Config.h"
#include "../action/ActionRunner.h"
#include "LogForwarder.h"
#include "StepRunner.h"

#include <string>
#include <vector>
#include <atomic>

namespace runner {

/// Executes one task end-to-end.
class TaskExecutor {
public:
    /// @param client     RunnerClient for UpdateTask/UpdateLog
    /// @param task       the task to execute
    /// @param cfg        runner configuration (name, capacity, etc.)
    TaskExecutor(IRunnerClient& client,
                 TaskDto        task,
                 const Config&  cfg);

    ~TaskExecutor() = default;

    // Non-copyable
    TaskExecutor(const TaskExecutor&)            = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;

    /// Execute the task.  Blocks until completion.
    /// Returns true if the job succeeded, false on failure.
    bool execute();

    /// Request cancellation of the running task.
    /// Also kills the currently-running step's child process.
    void cancel() {
        cancelled_.store(true);
        // Kill any currently-running step's subprocess.
        StepRunner* sr = active_step_runner_.load();
        if (sr) sr->cancel();
    }

    int64_t taskId() const { return task_.id; }

    /// Job-level outputs evaluated after execution.
    /// Only populated after execute() returns.
    /// Key = output name, Value = evaluated expression string.
    const std::vector<std::pair<std::string,std::string>>& jobOutputs() const {
        return job_outputs_;
    }

private:
    IRunnerClient& client_;
    TaskDto       task_;
    const Config& cfg_;
    std::atomic<bool> cancelled_{false};

    // Pointer to the currently-running StepRunner (set during execute()).
    // Accessed from cancel() which may be called from another thread.
    std::atomic<StepRunner*> active_step_runner_{nullptr};

    // Evaluated job-level outputs (populated by execute()).
    std::vector<std::pair<std::string,std::string>> job_outputs_;

    // ── Internal helpers ─────────────────────────────────────────────────

    /// Report task start to the server.
    void reportStart(int64_t started_at_s);

    /// Report final task result to the server.
    void reportEnd(bool success,
                   int64_t started_at_s,
                   int64_t stopped_at_s,
                   const std::vector<StepStateDto>& steps,
                   const std::vector<std::pair<std::string,std::string>>& job_outputs = {});

    /// Create a unique workspace directory for this task.
    std::string createWorkspace();

    /// Remove the workspace directory.
    void cleanupWorkspace(const std::string& path);

    /// Build the base environment for all steps.
    std::vector<std::pair<std::string,std::string>> buildBaseEnv(
        const std::string& workspace) const;
};

} // namespace runner
