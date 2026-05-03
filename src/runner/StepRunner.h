#pragma once
// StepRunner.h — Executes a single step within a job
//
// Handles both 'run:' (inline shell script) and 'uses:' (action reference)
// steps.

#include "../workflow/WorkflowParser.h"
#include "../workflow/ExprEvaluator.h"
#include "../process/ProcessSpawner.h"
#include "../process/EnvManager.h"
#include "../action/ActionRunner.h"
#include "LogForwarder.h"

#include <memory>

#include <string>
#include <map>
#include <functional>

namespace runner {

/// Result of executing one step.
struct StepRunResult {
    bool success     = false;
    int  exit_code   = -1;
    std::map<std::string,std::string> outputs;  ///< from $GITHUB_OUTPUT
};

/// Runs a single step, capturing output and managing protocol files.
class StepRunner {
public:
    /// @param workspace_dir       job workspace root (absolute path)
    /// @param shell               default shell ("/bin/sh" or from job defaults)
    /// @param action_cache        action cache for 'uses:' steps
    /// @param gitea_url           Gitea URL (fallback for fetching actions)
    /// @param default_actions_url Primary URL for remote actions (e.g. "https://github.com")
    /// @param actions_cache_dir   directory for the owned ActionCache (default: work_dir/actions_cache)
    StepRunner(std::string workspace_dir,
               std::string default_shell,
               ActionCache* action_cache = nullptr,
               std::string gitea_url = "",
               std::string default_actions_url = "https://github.com",
               std::string actions_cache_dir = "");

    /// Execute one step.
    ///
    /// @param step           the step to execute
    /// @param expr_ctx       current expression context (for ${{ }})
    /// @param env_mgr        environment file manager
    /// @param log            log forwarder (receives stdout/stderr)
    /// @param base_env       base environment variables
    /// @param remaining_s    remaining job budget in seconds (0 = no job-level limit)
    /// @param post_queue     accumulates deferred post: scripts (may be null)
    ///
    /// @returns StepRunResult (success = true if exit_code == 0 or step is skipped)
    StepRunResult run(const Step&         step,
                      const ExprContext&   expr_ctx,
                      EnvManager&          env_mgr,
                      LogForwarder&        log,
                      const std::vector<std::pair<std::string,std::string>>& base_env,
                      int                  remaining_s  = 0,
                      std::vector<PostScript>* post_queue = nullptr);

    /// Cancel a running step (sends SIGTERM to child).
    void cancel();

private:
    std::string  workspace_dir_;
    std::string  default_shell_;
    std::string  gitea_url_;
    std::string  default_actions_url_;
    ProcessSpawner spawner_;
    std::unique_ptr<ActionCache>  owned_cache_;  // if no external cache provided
    ActionCache*                  action_cache_;  // may point to owned_cache_

    StepRunResult runScript(const Step&         step,
                             const std::string&   script,
                             const std::string&   shell,
                             const std::string&   work_dir,
                             const std::vector<std::string>& env,
                             EnvManager&          env_mgr,
                             LogForwarder&        log,
                             int                  job_remaining_s = 0);

    StepRunResult runAction(const Step&         step,
                             const ExprContext&   expr_ctx,
                             EnvManager&          env_mgr,
                             LogForwarder&        log,
                             std::vector<PostScript>* post_queue = nullptr);

    std::string resolveShell(const Step& step) const;
};

} // namespace runner
