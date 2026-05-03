#pragma once
// ContextBuilder.h — Builds the ExprContext for a job run
//
// Populates the well-known GitHub Actions context objects:
//   github.*    — repository, workflow, ref, event, etc.
//   env.*       — job-level environment variables
//   runner.*    — runner name, OS, arch
//   steps.*     — outputs from completed steps
//   secrets.*   — secret values (never logged)
//   vars.*      — repository/org variables

#include "ExprEvaluator.h"
#include "../client/RunnerClient.h"
#include "../config/Config.h"

#include <string>
#include <map>
#include <vector>

namespace runner {

/// Builds an ExprContext from all sources available at job start.
class ContextBuilder {
public:
    ContextBuilder() = default;

    /// Set the task DTO (provides context map, secrets, vars from server).
    ContextBuilder& withTask(const TaskDto& task);

    /// Set runner identity (from Config + RunnerState).
    ContextBuilder& withRunnerInfo(const std::string& name,
                                    const std::string& os   = "haiku",
                                    const std::string& arch = "x86_64");

    /// Set the runner's work directory root (from Config::work_dir).
    /// Used to populate runner.temp and runner.tool_cache context values.
    /// If not called, these default to the system temp directory.
    ContextBuilder& withWorkDir(const std::string& work_dir);

    /// Set job-level environment (workflow env + job env merged).
    ContextBuilder& withJobEnv(const std::map<std::string,std::string>& env);

    /// Record a completed step's outputs (call after each step finishes).
    ContextBuilder& addStepOutputs(const std::string& step_id,
                                    int                result,
                                    const std::map<std::string,std::string>& outputs);

    /// Add a matrix combination's values (e.g. matrix.os = "haiku").
    ContextBuilder& withMatrix(const std::map<std::string,std::string>& combination);

    /// Build the context.
    ExprContext build() const;

    /// Build and return a copy with updated steps context.
    ExprContext buildUpdated() const { return build(); }

private:
    const TaskDto*                         task_     = nullptr;
    std::string                            runner_name_, runner_os_, runner_arch_;
    std::string                            work_dir_;   ///< from Config::work_dir
    std::map<std::string,std::string>      job_env_;
    std::map<std::string,std::string>      matrix_;

    struct StepResult {
        std::string id;
        int         result = 0; // 0=unspecified, 1=success, 2=failure, 3=cancelled, 4=timeout
        std::map<std::string,std::string> outputs;
    };
    std::vector<StepResult> step_results_;

    static std::string resultName(int r) {
        switch (r) {
            case 1: return "success";
            case 2: return "failure";
            case 3: return "cancelled";
            case 4: return "failure"; // timeout → failure for conditions
            default: return "";
        }
    }
};

} // namespace runner
