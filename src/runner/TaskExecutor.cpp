// TaskExecutor.cpp — Full task orchestration
#include "TaskExecutor.h"
#include "../util/Logger.h"

#include "../workflow/WorkflowParser.h"
#include "../workflow/ExprEvaluator.h"
#include "../workflow/ContextBuilder.h"
#include "../process/EnvManager.h"
#include "../process/ProcessSpawner.h"
#include "StepRunner.h"

#include <filesystem>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <random>

#include <nlohmann/json.hpp>

namespace runner {

namespace fs = std::filesystem;

// ─── Helpers ──────────────────────────────────────────────────────────────

static int64_t now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string randomHex(int bytes) {
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 255);
    std::ostringstream ss;
    for (int i = 0; i < bytes; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << dist(rng);
    }
    return ss.str();
}

// ─── Constructor ──────────────────────────────────────────────────────────

TaskExecutor::TaskExecutor(IRunnerClient& client,
                             TaskDto        task,
                             const Config&  cfg)
    : client_(client)
    , task_(std::move(task))
    , cfg_(cfg)
{}

// ─── createWorkspace ──────────────────────────────────────────────────────

std::string TaskExecutor::createWorkspace() {
    // Use /tmp/act_runner_<task_id>_<random> for isolation
    std::string path = "/tmp/act_runner_"
                     + std::to_string(task_.id)
                     + "_" + randomHex(4);
    fs::create_directories(path);
    return path;
}

void TaskExecutor::cleanupWorkspace(const std::string& path) {
    try {
        fs::remove_all(path);
    } catch (const std::exception& e) {
        LOG_WARN("TaskExecutor", "Workspace cleanup failed for " << path << ": " << e.what());
    }
}

// ─── buildBaseEnv ─────────────────────────────────────────────────────────

std::vector<std::pair<std::string,std::string>>
TaskExecutor::buildBaseEnv(const std::string& workspace) const
{
    std::vector<std::pair<std::string,std::string>> env;

    // Standard GitHub Actions environment variables
    env.emplace_back("GITHUB_WORKSPACE",          workspace);
    env.emplace_back("GITHUB_ACTION_PATH",        workspace);
    env.emplace_back("RUNNER_WORKSPACE",          workspace);

    // Runtime token for toolkit callbacks
    if (!task_.gitea_runtime_token.empty()) {
        env.emplace_back("ACTIONS_RUNTIME_TOKEN",  task_.gitea_runtime_token);
        env.emplace_back("ACTIONS_RUNTIME_URL",    cfg_.gitea_url + "/_git/");
        // GITHUB_TOKEN is the standard name used by actions/checkout, gh CLI,
        // and virtually every published action that calls the Gitea API.
        env.emplace_back("GITHUB_TOKEN",            task_.gitea_runtime_token);
    }

    // Task context values — map flat Gitea context keys to GITHUB_* env vars.
    std::string github_ref;       // needed below for REF_TYPE derivation
    std::string github_actor;     // needed for TRIGGERING_ACTOR
    std::string github_repo;      // needed for REPOSITORY_OWNER
    for (auto& [k, v] : task_.context) {
        if (k == "sha")          env.emplace_back("GITHUB_SHA",          v);
        else if (k == "ref")   { env.emplace_back("GITHUB_REF",          v); github_ref  = v; }
        else if (k == "ref_name") env.emplace_back("GITHUB_REF_NAME",    v);
        else if (k == "repo" || k == "repository") {
                                  env.emplace_back("GITHUB_REPOSITORY",  v); github_repo = v; }
        else if (k == "workflow") env.emplace_back("GITHUB_WORKFLOW",    v);
        else if (k == "run_id")   env.emplace_back("GITHUB_RUN_ID",      v);
        else if (k == "run_number") env.emplace_back("GITHUB_RUN_NUMBER",v);
        else if (k == "actor")  { env.emplace_back("GITHUB_ACTOR",       v); github_actor = v; }
        else if (k == "event_name") env.emplace_back("GITHUB_EVENT_NAME",v);
        else if (k == "server_url") env.emplace_back("GITHUB_SERVER_URL",v);
        else if (k == "api_url")  env.emplace_back("GITHUB_API_URL",     v);
        else if (k == "graphql_url") env.emplace_back("GITHUB_GRAPHQL_URL", v);
        else if (k == "head_ref") env.emplace_back("GITHUB_HEAD_REF",    v);
        else if (k == "base_ref") env.emplace_back("GITHUB_BASE_REF",    v);
    }

    // GITHUB_JOB — the job id key from the workflow YAML.
    // task_.machine is set by the server to the job id string.
    env.emplace_back("GITHUB_JOB", task_.machine);

    // GITHUB_REF_TYPE — "branch" or "tag", derived from GITHUB_REF.
    // actions/checkout@v4 reads this to choose git checkout mode.
    if (!github_ref.empty()) {
        if (github_ref.rfind("refs/tags/", 0) == 0)
            env.emplace_back("GITHUB_REF_TYPE", "tag");
        else
            env.emplace_back("GITHUB_REF_TYPE", "branch");
    }

    // GITHUB_REPOSITORY_OWNER — the org/user part of "owner/repo".
    // actions/checkout uses this to construct the clone URL.
    if (!github_repo.empty()) {
        auto slash = github_repo.find('/');
        if (slash != std::string::npos)
            env.emplace_back("GITHUB_REPOSITORY_OWNER", github_repo.substr(0, slash));
    }

    // GITHUB_TRIGGERING_ACTOR — the actor that triggered the workflow.
    // Defaults to GITHUB_ACTOR (same person in most cases).
    if (!github_actor.empty())
        env.emplace_back("GITHUB_TRIGGERING_ACTOR", github_actor);

    // RUNNER_ENVIRONMENT — "self-hosted" or "github-hosted".
    // actions/setup-* check this before installing tools; they skip setup
    // on github-hosted runners assuming tools are pre-installed.
    env.emplace_back("RUNNER_ENVIRONMENT", "self-hosted");

    // Gitea server URL
    env.emplace_back("GITEA_SERVER_URL", cfg_.gitea_url);
    env.emplace_back("CI",               "true");
    env.emplace_back("GITHUB_ACTIONS",   "true");

    // Local cache server URL (actions/cache API) — only set if the daemon
    // started the CacheServer and populated cache_url_runtime.
    if (!cfg_.cache_url_runtime.empty()) {
        env.emplace_back("ACTIONS_CACHE_URL",   cfg_.cache_url_runtime);
        env.emplace_back("ACTIONS_RESULTS_URL", cfg_.cache_url_runtime);
    }

    return env;
}

// ─── extractEventInfo ─────────────────────────────────────────────────────

/// Extract event_name and event_payload JSON from task context.
static std::pair<std::string,std::string> extractEventInfo(
    const std::vector<std::pair<std::string,std::string>>& context)
{
    std::string event_name;
    std::string event_payload;
    for (auto& [k, v] : context) {
        if (k == "event_name")   event_name    = v;
        if (k == "event")        event_payload = v;
        if (k == "event_payload") event_payload = v;
    }
    return {event_name, event_payload};
}

/// Extract matrix combination from task context.  Gitea may send it as:
///   1. a "matrix" key with a JSON object value, e.g. {"os":"haiku","go":"1.21"}
///   2. flattened keys like "matrix.os" → "haiku"
/// We try both and merge.
static std::map<std::string,std::string> extractMatrixCombo(
    const std::vector<std::pair<std::string,std::string>>& context)
{
    std::map<std::string,std::string> combo;
    for (auto& [k, v] : context) {
        if (k == "matrix") {
            // JSON object → flatten
            try {
                auto j = nlohmann::json::parse(v);
                if (j.is_object()) {
                    for (auto& [jk, jv] : j.items()) {
                        combo[jk] = jv.is_string() ? jv.get<std::string>() : jv.dump();
                    }
                }
            } catch (...) { /* not JSON → ignore */ }
        } else if (k.rfind("matrix.", 0) == 0) {
            combo[k.substr(7)] = v;
        }
    }
    return combo;
}

// ─── reportStart ──────────────────────────────────────────────────────────

void TaskExecutor::reportStart(int64_t started_at_s) {
    try {
        client_.updateTask(task_.id, 0 /*RESULT_UNSPECIFIED*/, {}, started_at_s, 0);
    } catch (const std::exception& e) {
        LOG_WARN("TaskExecutor", "reportStart failed: " << e.what());
    }
}

// ─── reportEnd ────────────────────────────────────────────────────────────

void TaskExecutor::reportEnd(bool success,
                              int64_t started_at_s,
                              int64_t stopped_at_s,
                              const std::vector<StepStateDto>& steps)
{
    int result = success ? 1 /*RESULT_SUCCESS*/ : 2 /*RESULT_FAILURE*/;
    if (cancelled_.load()) result = 3; /*RESULT_CANCELLED*/

    try {
        client_.updateTask(task_.id, result, steps, started_at_s, stopped_at_s);
    } catch (const std::exception& e) {
        LOG_WARN("TaskExecutor", "reportEnd failed: " << e.what());
    }
}

// ─── execute ──────────────────────────────────────────────────────────────

bool TaskExecutor::execute() {
    LOG_INFO("TaskExecutor", "Starting task " << task_.id
             << " (job: " << task_.machine << ")");

    int64_t started_at = now_s();
    bool overall_success = true;

    // ── Parse workflow ────────────────────────────────────────────────────
    Workflow wf;
    try {
        wf = parseWorkflow(task_.workflow_payload);
    } catch (const std::exception& e) {
        LOG_ERROR("TaskExecutor", "Workflow parse error for task "
                  << task_.id << ": " << e.what());
        reportStart(started_at);
        reportEnd(false, started_at, now_s(), {});
        return false;
    }

    // ── Find job ─────────────────────────────────────────────────────────
    // task_.machine identifies the job ID
    Job* job = nullptr;
    if (!task_.machine.empty() && wf.jobs.count(task_.machine)) {
        job = &wf.jobs.at(task_.machine);
    } else if (wf.jobs.size() == 1) {
        job = &wf.jobs.begin()->second;
    }

    if (!job) {
        LOG_ERROR("TaskExecutor", "Cannot find job '" << task_.machine
                  << "' in workflow for task " << task_.id);
        reportStart(started_at);
        reportEnd(false, started_at, now_s(), {});
        return false;
    }

    // ── Setup ─────────────────────────────────────────────────────────────
    std::string workspace = createWorkspace();

    // Extract event info for EnvManager + context
    auto [event_name, event_payload] = extractEventInfo(task_.context);

    EnvManager env_mgr(workspace + "/_runner");
    env_mgr.setup(event_payload, event_name);

    // Log forwarder — stderr (via Logger) + batched UpdateLog
    auto console_cb = [](const LogLine& ll) {
        LOG_INFO("Job", ll.content);
    };
    LogForwarder log_fwd(client_, task_.id, 50, 1000, console_cb);

    // Report start
    reportStart(started_at);

    // ── Build base environment ─────────────────────────────────────────
    auto base_env = buildBaseEnv(workspace);

    // ── Register secrets for log masking ─────────────────────────────
    // All secret values and the runtime token must be masked in log output
    // to prevent accidental credential exposure in the job log.
    for (auto& [k, v] : task_.secrets) {
        log_fwd.addSecret(v);
    }
    if (!task_.gitea_runtime_token.empty()) {
        log_fwd.addSecret(task_.gitea_runtime_token);
    }

    // ── Build initial context ─────────────────────────────────────────
    std::map<std::string,std::string> job_env_map;
    for (auto& [k, v] : base_env) job_env_map[k] = v;

    ContextBuilder ctx_builder;
    ctx_builder
        .withTask(task_)
        .withRunnerInfo(cfg_.name)
        .withJobEnv(job_env_map);

    // ── Extract matrix combination (if any) from task.context ─────────
    // The Gitea server pre-expands matrix jobs and dispatches one task per
    // combination; the values for this combination arrive via task.context.
    auto matrix_combo = extractMatrixCombo(task_.context);
    if (!matrix_combo.empty()) {
        ctx_builder.withMatrix(matrix_combo);
        for (auto& [k, v] : matrix_combo) {
            LOG_DEBUG("TaskExecutor", "matrix." << k << " = " << v);
        }
    }

    ExprContext initial_ctx = ctx_builder.build();
    ExprEvaluator initial_eval(initial_ctx);

    // ── Evaluate job-level if: condition ──────────────────────────────
    if (!job->if_condition.empty()) {
        if (!initial_eval.evaluateCondition(job->if_condition)) {
            LOG_INFO("TaskExecutor", "Job skipped by if: condition: " << job->if_condition);
            log_fwd.append("##[notice]Job skipped due to 'if:' condition");
            log_fwd.finish();
            env_mgr.cleanup();
            cleanupWorkspace(workspace);
            // Report as success (skipped = OK per GitHub Actions semantics)
            reportEnd(true, started_at, now_s(), {});
            return true;
        }
    }

    // ── Merge workflow/job env with interpolation ──────────────────────
    // Env values may contain ${{ }} expressions; evaluate them now.
    for (auto& [k, v] : wf.env) {
        base_env.emplace_back(k, initial_eval.interpolate(v));
    }
    for (auto& [k, v] : job->env) {
        base_env.emplace_back(k, initial_eval.interpolate(v));
    }

    // Rebuild context with interpolated env
    std::map<std::string,std::string> full_env_map;
    for (auto& [k, v] : base_env) full_env_map[k] = v;
    ctx_builder.withJobEnv(full_env_map);

    // ── Determine default shell ────────────────────────────────────────
    std::string default_shell = "/bin/sh";
    if (!job->default_shell.empty())  default_shell = job->default_shell;
    else if (!wf.default_shell.empty()) default_shell = wf.default_shell;
    if (default_shell == "bash") default_shell = "/bin/bash";

    // ── Step runner ────────────────────────────────────────────────────
    StepRunner step_runner(workspace, default_shell);

    // Deferred post: scripts — filled by StepRunner/ActionRunner when a JS
    // action has a post: hook.  Drained in reverse order after all steps.
    std::vector<PostScript> post_queue;

    // Track step states for UpdateTask
    std::vector<StepStateDto> step_states;
    int64_t log_line_index = 0;

    // ── Job-level timeout ─────────────────────────────────────────────────
    // GitHub Actions default = 360 minutes (6 h) if not specified.
    // Precedence: job-level > task-level (from server/workflow timeout_minutes) > 360 min default.
    // task_.timeout is in seconds (from the Gitea server / local orchestrator).
    int task_timeout_minutes = (task_.timeout > 0) ? static_cast<int>(task_.timeout / 60) : 360;
    const int job_timeout_minutes = (job->timeout_minutes > 0) ? job->timeout_minutes
                                                                : task_timeout_minutes;
    // Ensure we never have a 0 timeout — that would kill every step immediately.
    const int effective_timeout_minutes = (job_timeout_minutes > 0) ? job_timeout_minutes : 360;
    const int job_timeout_s       = effective_timeout_minutes * 60;
    const auto job_start_tp       = std::chrono::steady_clock::now();

    // ── Execute steps ─────────────────────────────────────────────────
    for (size_t i = 0; i < job->steps.size(); ++i) {
        const Step& step = job->steps[i];

        if (cancelled_.load()) {
            log_fwd.append("##[warning]Job cancelled — skipping remaining steps");
            overall_success = false;
            break;
        }

        // ── Check job timeout before dispatching this step ────────────
        auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - job_start_tp).count();
        int remaining_s = job_timeout_s - static_cast<int>(elapsed_s);
        if (remaining_s <= 0) {
            LOG_ERROR("TaskExecutor", "Job timeout (" << effective_timeout_minutes
                      << " min) exceeded — aborting remaining steps");
            log_fwd.append("##[error]Job timed out after "
                           + std::to_string(effective_timeout_minutes) + " minute(s)");
            step_runner.cancel();
            overall_success = false;
            break;
        }

        ExprContext expr_ctx = ctx_builder.build();

        int64_t step_start = now_s();
        log_line_index = log_fwd.currentLogIndex();

        StepRunResult step_result = step_runner.run(
            step, expr_ctx, env_mgr, log_fwd, base_env, remaining_s, &post_queue);

        int64_t step_end = now_s();

        // Record step state — flush the forwarder so next_index_ is up-to-date
        // before we compute log_length.  This is a best-effort sync: the
        // background thread may still have in-flight batches, but waiting
        // longer would delay the updateTask RPC.
        // Use currentLogIndex() (atomic) for the best available count.
        StepStateDto ss;
        ss.id         = static_cast<int64_t>(i + 1); // 1-based
        ss.result     = step_result.success ? 1 /*SUCCESS*/ : 2 /*FAILURE*/;
        ss.started_at_s = step_start;
        ss.stopped_at_s = step_end;
        ss.log_index  = log_line_index;
        ss.log_length = log_fwd.currentLogIndex() - log_line_index;
        step_states.push_back(ss);

        // Update context with step outputs
        ctx_builder.addStepOutputs(step.id, ss.result, step_result.outputs);

        // Refresh the env context so that $GITHUB_ENV additions from this step
        // are visible in the next step's if: conditions and run: interpolations.
        {
            std::map<std::string,std::string> live_env;
            for (auto& [k, v] : env_mgr.currentEnv()) live_env[k] = v;
            ctx_builder.withJobEnv(live_env);
        }

        // Report step completion to server
        try {
            client_.updateTask(task_.id, 0 /*still running*/, step_states,
                               started_at, 0);
        } catch (...) { /* non-fatal */ }

        // Stop on failure (unless continue-on-error)
        if (!step_result.success && !step.continue_on_error) {
            log_fwd.append("##[error]Step '" + step.id + "' failed — stopping job");
            overall_success = false;
            break;
        }
    }

    // ── Run deferred post: scripts ────────────────────────────────────────
    // GitHub Actions runs post: scripts in reverse step order, after the job
    // finishes (success or failure).  This allows actions like actions/cache
    // to save cache entries even when a later step fails.
    if (!post_queue.empty()) {
        LOG_INFO("TaskExecutor", "Running " << post_queue.size()
                 << " deferred post: script(s)");
        // Drain in reverse registration order (= reverse step order)
        for (int pi = static_cast<int>(post_queue.size()) - 1; pi >= 0; --pi) {
            const PostScript& ps = post_queue[static_cast<size_t>(pi)];
            LOG_DEBUG("TaskExecutor", "post: " << ps.script_path);
            log_fwd.append("##[group]Post: " + ps.script_path);

            ProcessSpawner post_spawner;
            auto on_out = [&log_fwd](const std::string& line, bool /*is_stderr*/) {
                log_fwd.append(line);
            };
            ProcessResult post_proc = post_spawner.run(
                ps.node_bin, ps.script_path,
                ps.working_dir, ps.env, on_out,
                /*timeout_s=*/300   // 5-minute cap per post: script
            );

            if (post_proc.exit_code != 0) {
                LOG_WARN("TaskExecutor", "post: script exited "
                         << post_proc.exit_code << " — " << ps.script_path);
                log_fwd.append("##[warning]post: script failed with exit code "
                               + std::to_string(post_proc.exit_code));
            }
            log_fwd.append("##[endgroup]");
        }
    }

    // ── Evaluate job outputs ─────────────────────────────────────────────
    // job.outputs: map of name → "${{ steps.X.outputs.Y }}" expressions
    // Evaluate them using the final context (which has all step outputs).
    // Results are stored in job_outputs_ so the caller (WorkflowOrchestrator)
    // can propagate them via needs_context to downstream jobs.
    if (!job->outputs.empty()) {
        ExprContext final_ctx = ctx_builder.build();
        ExprEvaluator final_eval(final_ctx);
        for (auto& [oname, oexpr] : job->outputs) {
            std::string oval = final_eval.interpolate(oexpr);
            LOG_DEBUG("TaskExecutor", "Job output '" << oname << "' = '" << oval << "'");
            job_outputs_.emplace_back(oname, oval);
        }
        LOG_INFO("TaskExecutor", "Evaluated " << job_outputs_.size()
                 << " job output(s)");
    }

    // ── Handle job-level continue-on-error ────────────────────────────────
    if (!overall_success && job->continue_on_error) {
        LOG_INFO("TaskExecutor", "Job continue-on-error: reporting success despite step failure");
        overall_success = true;
    }

    // ── Finish ────────────────────────────────────────────────────────
    env_mgr.cleanup();
    log_fwd.finish();  // flush remaining logs + no_more=true

    int64_t stopped_at = now_s();
    cleanupWorkspace(workspace);

    reportEnd(overall_success, started_at, stopped_at, step_states);

    LOG_INFO("TaskExecutor", "Task " << task_.id << " completed: "
             << (overall_success ? "SUCCESS" : "FAILURE"));

    return overall_success;
}

} // namespace runner
