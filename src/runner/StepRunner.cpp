// StepRunner.cpp — Single step execution
#include "StepRunner.h"

#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace runner {

namespace fs = std::filesystem;

StepRunner::StepRunner(std::string workspace_dir,
                       std::string default_shell,
                       ActionCache* action_cache,
                       std::string gitea_url,
                       std::string default_actions_url)
    : workspace_dir_(std::move(workspace_dir))
    , default_shell_(default_shell.empty() ? "/bin/sh" : std::move(default_shell))
    , gitea_url_(std::move(gitea_url))
    , default_actions_url_(default_actions_url.empty()
                           ? "https://github.com"
                           : std::move(default_actions_url))
{
    if (action_cache) {
        action_cache_ = action_cache;
    } else {
        owned_cache_ = std::make_unique<ActionCache>("/tmp/act_runner_actions");
        action_cache_ = owned_cache_.get();
    }
}

std::string StepRunner::resolveShell(const Step& step) const {
    if (!step.shell.empty()) {
        if (step.shell == "bash") return "/bin/bash";
        if (step.shell == "sh")   return "/bin/sh";
        // Absolute path — use directly
        if (step.shell[0] == '/') return step.shell;
        return step.shell;
    }
    return default_shell_;
}

StepRunResult StepRunner::run(
    const Step&         step,
    const ExprContext&   expr_ctx,
    EnvManager&          env_mgr,
    LogForwarder&        log,
    const std::vector<std::pair<std::string,std::string>>& base_env,
    int                  remaining_s,
    std::vector<PostScript>* post_queue)
{
    ExprEvaluator eval(expr_ctx);

    // ── Evaluate 'if:' condition ─────────────────────────────────────────
    if (!step.if_condition.empty()) {
        if (!eval.evaluateCondition(step.if_condition)) {
            log.append("##[debug]Step '" + step.id + "' skipped (if: " +
                       step.if_condition + ")");
            StepRunResult r;
            r.success   = true;   // skipped = success in GitHub Actions
            r.exit_code = 0;
            return r;
        }
    }

    // ── Log step header ──────────────────────────────────────────────────
    std::string display_name = step.name.empty() ? step.id : eval.interpolate(step.name);
    log.append("##[group]" + display_name);

    // ── Build step environment ───────────────────────────────────────────
    // Priority: base_env < job_env (already in base) < step.env < protocol vars
    std::vector<std::pair<std::string,std::string>> step_env_pairs = base_env;

    // Add step-specific env (with expression interpolation)
    for (auto& [k, v] : step.env) {
        step_env_pairs.emplace_back(k, eval.interpolate(v));
    }

    // Merge with EnvManager accumulated env
    auto mgr_env = env_mgr.currentEnv();
    step_env_pairs.insert(step_env_pairs.end(), mgr_env.begin(), mgr_env.end());

    // Build flat vector<string> "KEY=VALUE" for ProcessSpawner
    auto flat_env = buildEnv(step_env_pairs);

    // ── Determine working directory ────────────────────────────────────
    std::string work_dir = workspace_dir_;
    if (!step.working_dir.empty()) {
        work_dir = eval.interpolate(step.working_dir);
        if (work_dir[0] != '/') {
            work_dir = workspace_dir_ + "/" + work_dir;
        }
    }
    fs::create_directories(work_dir);

    StepRunResult result;

    // ── Dispatch ─────────────────────────────────────────────────────────
    if (!step.run.empty()) {
        // Inline script
        std::string script = eval.interpolate(step.run);
        std::string shell  = resolveShell(step);
        result = runScript(step, script, shell, work_dir, flat_env, env_mgr, log,
                           remaining_s);
    } else if (!step.uses.empty()) {
        result = runAction(step, expr_ctx, env_mgr, log, post_queue);
    } else {
        log.append("##[error]Step '" + step.id + "' has neither 'run' nor 'uses'");
        result.success   = false;
        result.exit_code = 1;
    }

    log.append("##[endgroup]");

    // ── Collect $GITHUB_OUTPUT ───────────────────────────────────────────
    // Merge: for 'uses:' steps, ActionRunner already collects outputs from
    // composite/JS sub-steps and returns them in result.outputs.
    // We additionally parse any outputs written directly to $GITHUB_OUTPUT
    // during this step (e.g. a JS action that uses @actions/core directly).
    // File-based outputs take precedence over what ActionRunner accumulated.
    {
        auto file_outputs = env_mgr.parseOutputs();
        // Start with what ActionRunner returned (may be non-empty for composite)
        for (auto& [k, v] : file_outputs) {
            result.outputs[k] = v;   // file wins over in-memory if both set
        }
    }

    // ── Apply $GITHUB_ENV and $GITHUB_PATH ───────────────────────────────
    env_mgr.applyEnvChanges();
    env_mgr.applyPathChanges();
    env_mgr.resetBetweenSteps();

    return result;
}

StepRunResult StepRunner::runScript(
    const Step&         step,
    const std::string&   script,
    const std::string&   shell,
    const std::string&   work_dir,
    const std::vector<std::string>& env,
    EnvManager&          env_mgr,
    LogForwarder&        log,
    int                  job_remaining_s)
{
    // Write the script to a temp file for better error reporting and clean exec.
    std::string script_path = (fs::path(env_mgr.githubOutputPath()).parent_path()
                               / ("_step_" + step.id + ".sh")).string();
    {
        std::ofstream sf(script_path);
        // For bash: use pipefail so pipeline failures propagate correctly.
        // For POSIX sh: only set -e (pipefail is bash-specific).
        if (shell.find("bash") != std::string::npos) {
            sf << "#!/bin/bash\nset -eo pipefail\n";
        } else {
            sf << "#!/bin/sh\nset -e\n";
        }
        sf << script << "\n";
    }
    // Make executable so we exec it directly without -c
    chmod(script_path.c_str(), 0700);

    auto on_output = [&log](const std::string& line, bool is_stderr) {
        log.append(line, is_stderr);
    };

    // Step-level timeout (seconds); 0 means no step-level limit.
    int step_timeout_s = step.timeout_minutes > 0 ? step.timeout_minutes * 60 : 0;

    // Effective timeout = min(step_timeout, job_remaining) — whichever fires first.
    // If either is 0 (= unlimited), use the other; if both 0, unlimited.
    int timeout_s = 0;
    if (step_timeout_s > 0 && job_remaining_s > 0)
        timeout_s = std::min(step_timeout_s, job_remaining_s);
    else if (step_timeout_s > 0)
        timeout_s = step_timeout_s;
    else if (job_remaining_s > 0)
        timeout_s = job_remaining_s;

    // Execute the script file directly as: <shell> <script_path>
    // This is cleaner than "shell -c 'set -e; . script_path'" because:
    //   - set -e lives inside the script itself (line 1)
    //   - No nested quoting or sourcing quirks
    //   - Error line numbers in messages match the script file
    ProcessResult proc = spawner_.run(
        shell,
        script_path,
        work_dir,
        env,
        on_output,
        timeout_s
    );

    // Cleanup script file
    fs::remove(script_path);

    StepRunResult r;
    r.exit_code = proc.exit_code;
    r.success   = (proc.exit_code == 0);

    if (proc.killed) {
        std::string msg = "##[error]Step killed by signal " + std::to_string(proc.signal);
        // Signal 7 (SIGKILLTHR) is a Haiku-specific kernel bug that occasionally
        // fires when threads call posix_spawn() concurrently. It is transient and
        // not indicative of a workflow problem.  Suggest a workaround.
        if (proc.signal == 7) {
            msg += " (SIGKILLTHR — Haiku posix_spawn race; try: act_runner run --retry 3)";
        }
        log.append(msg);
    } else if (proc.exit_code != 0) {
        log.append("##[error]Process completed with exit code " +
                   std::to_string(proc.exit_code));
    }

    return r;
}

StepRunResult StepRunner::runAction(
    const Step&         step,
    const ExprContext&   expr_ctx,
    EnvManager&          env_mgr,
    LogForwarder&        log,
    std::vector<PostScript>* post_queue)
{
    ActionRunner action_runner(workspace_dir_, *action_cache_,
                                default_shell_, gitea_url_,
                                default_actions_url_);

    // We need base_env here — rebuild it from env_mgr's current env
    auto base_env = env_mgr.currentEnv();

    try {
        auto result = action_runner.run(step, expr_ctx, env_mgr, log, base_env,
                                        post_queue);
        StepRunResult r;
        r.success   = result.success;
        r.exit_code = result.exit_code;
        r.outputs   = std::move(result.outputs);
        return r;
    } catch (const std::exception& e) {
        log.append(std::string("##[error]Action failed: ") + e.what());
        StepRunResult r;
        r.success   = step.continue_on_error;
        r.exit_code = 1;
        return r;
    }
}

void StepRunner::cancel() {
    spawner_.kill();
}

} // namespace runner
