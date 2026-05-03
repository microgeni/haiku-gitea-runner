// ContextBuilder.cpp — Populates the ExprContext for a job run
#include "ContextBuilder.h"

#include <nlohmann/json.hpp>
#include <stdexcept>
#include "../util/Logger.h"

namespace runner {

using json = nlohmann::json;

ContextBuilder& ContextBuilder::withTask(const TaskDto& task) {
    task_ = &task;
    return *this;
}

ContextBuilder& ContextBuilder::withRunnerInfo(const std::string& name,
                                                const std::string& os,
                                                const std::string& arch)
{
    runner_name_ = name;
    runner_os_   = os;
    runner_arch_ = arch;
    return *this;
}

ContextBuilder& ContextBuilder::withJobEnv(const std::map<std::string,std::string>& env) {
    job_env_ = env;
    return *this;
}

ContextBuilder& ContextBuilder::addStepOutputs(
    const std::string& step_id,
    int result,
    const std::map<std::string,std::string>& outputs)
{
    step_results_.push_back({step_id, result, outputs});
    return *this;
}

ContextBuilder& ContextBuilder::withMatrix(const std::map<std::string,std::string>& combo) {
    matrix_ = combo;
    return *this;
}

ExprContext ContextBuilder::build() const {
    ExprContext ctx;

    // ── github.* context ─────────────────────────────────────────────────
    // The server provides this as JSON-encoded values in TaskDto::context
    if (task_) {
        for (auto& [k, v] : task_->context) {
            // Skip keys handled separately by the caller (TaskExecutor):
            //   - "matrix" / "matrix.*"  → withMatrix()
            //   - "event" / "event_name" / "event_payload" → env + EnvManager
            if (k == "matrix" || k.rfind("matrix.", 0) == 0) continue;
            if (k == "event" || k == "event_name" || k == "event_payload") {
                // event_name is still useful on github.event_name
                if (k == "event_name") ctx.setString("github.event_name", v);
                continue;
            }

            std::string path = k;
            // If the key doesn't contain a dot, prefix with "github."
            // (server sends flat map: "ref" → value)
            if (path.find('.') == std::string::npos) {
                // Try to parse if it's a JSON object (full context blob)
                try {
                    json j = json::parse(v);
                    if (j.is_object()) {
                        for (auto& [jk, jv] : j.items()) {
                            std::string jv_str = jv.is_string() ? jv.get<std::string>() : jv.dump();
                            ctx.setString("github." + jk, jv_str);
                        }
                        continue;
                    }
                } catch (...) {}
                ctx.setString("github." + path, v);
            } else {
                ctx.setString(path, v);
            }
        }

        // ── secrets.* ────────────────────────────────────────────────────
        for (auto& [k, v] : task_->secrets) {
            ctx.setString("secrets." + k, v);
        }

        // ── vars.* ───────────────────────────────────────────────────────
        for (auto& [k, v] : task_->vars) {
            ctx.setString("vars." + k, v);
        }
    }

    // ── runner.* context ─────────────────────────────────────────────────
    ctx.setString("runner.name",  runner_name_.empty() ? "haiku-runner" : runner_name_);
    ctx.setString("runner.os",    runner_os_.empty()   ? "haiku"        : runner_os_);
    ctx.setString("runner.arch",  runner_arch_.empty() ? "X64"          : runner_arch_);
    ctx.setString("runner.temp",  "/tmp");
    ctx.setString("runner.tool_cache", "/tmp/tool_cache");

    // github.token — the Gitea runtime token that actions use for API calls.
    // Exposed as ${{ github.token }} (the canonical reference in published actions).
    if (task_ && !task_->gitea_runtime_token.empty()) {
        ctx.setString("github.token", task_->gitea_runtime_token);
    }

    // ── env.* context ─────────────────────────────────────────────────────
    for (auto& [k, v] : job_env_) {
        ctx.setString("env." + k, v);
    }

    // ── steps.* context ───────────────────────────────────────────────────
    for (auto& sr : step_results_) {
        ctx.setString("steps." + sr.id + ".outcome",    resultName(sr.result));
        ctx.setString("steps." + sr.id + ".conclusion", resultName(sr.result));
        for (auto& [ok, ov] : sr.outputs) {
            ctx.setString("steps." + sr.id + ".outputs." + ok, ov);
        }
    }

    // ── matrix.* context ─────────────────────────────────────────────────
    for (auto& [k, v] : matrix_) {
        ctx.setString("matrix." + k, v);
    }

    // ── needs.* context ──────────────────────────────────────────────────
    // The server provides outputs from upstream (needs:) jobs via
    // TaskDto::needs_context.  Expose them as:
    //   needs.<job_id>.result              = "success"|"failure"|...
    //   needs.<job_id>.outputs.<out_name>  = <value>
    if (task_) {
        for (auto& [job_id, nc] : task_->needs_context) {
            ctx.setString("needs." + job_id + ".result", resultName(nc.result));
            for (auto& [ok, ov] : nc.outputs) {
                ctx.setString("needs." + job_id + ".outputs." + ok, ov);
            }
        }
    }

    return ctx;
}

} // namespace runner
