// WorkflowParser.cpp — YAML workflow parsing using yaml-cpp
#include "WorkflowParser.h"

#include <yaml-cpp/yaml.h>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace runner {

// ─── Helper: safe string ──────────────────────────────────────────────────

static std::string str(const YAML::Node& n) {
    if (!n || n.IsNull()) return "";
    return n.as<std::string>("");
}

static bool boolean(const YAML::Node& n, bool def = false) {
    if (!n || n.IsNull()) return def;
    try { return n.as<bool>(); } catch (...) { return def; }
}

static int integer(const YAML::Node& n, int def = 0) {
    if (!n || n.IsNull()) return def;
    try { return n.as<int>(); } catch (...) { return def; }
}

// ─── Parse helpers ────────────────────────────────────────────────────────

static std::map<std::string,std::string> parseStringMap(const YAML::Node& n) {
    std::map<std::string,std::string> result;
    if (!n || !n.IsMap()) return result;
    for (const auto& kv : n) {
        result[kv.first.as<std::string>()] = str(kv.second);
    }
    return result;
}

static std::vector<std::string> parseStringSeq(const YAML::Node& n) {
    std::vector<std::string> result;
    if (!n) return result;
    if (n.IsScalar()) {
        result.push_back(n.as<std::string>());
    } else if (n.IsSequence()) {
        for (const auto& v : n) result.push_back(str(v));
    }
    return result;
}

static Matrix parseMatrix(const YAML::Node& n) {
    Matrix m;
    if (!n || !n.IsMap()) return m;

    for (const auto& kv : n) {
        std::string key = kv.first.as<std::string>();
        if (key == "include") {
            if (kv.second.IsSequence()) {
                for (const auto& entry : kv.second) {
                    m.include.push_back(parseStringMap(entry));
                }
            }
        } else if (key == "exclude") {
            if (kv.second.IsSequence()) {
                for (const auto& entry : kv.second) {
                    m.exclude.push_back(parseStringMap(entry));
                }
            }
        } else {
            // Axis: key → list of values
            if (kv.second.IsSequence()) {
                for (const auto& v : kv.second) {
                    m.axes[key].push_back(str(v));
                }
            }
        }
    }
    return m;
}

static Step parseStep(const YAML::Node& n, int index) {
    Step s;

    s.id   = str(n["id"]);
    if (s.id.empty()) s.id = "step_" + std::to_string(index);

    s.name             = str(n["name"]);
    s.uses             = str(n["uses"]);
    s.run              = str(n["run"]);
    s.shell            = str(n["shell"]);
    s.working_dir      = str(n["working-directory"]);
    s.if_condition     = str(n["if"]);
    s.continue_on_error = boolean(n["continue-on-error"]);
    s.timeout_minutes  = integer(n["timeout-minutes"]);
    s.env              = parseStringMap(n["env"]);
    s.with             = parseStringMap(n["with"]);

    return s;
}

static Job parseJob(const std::string& id, const YAML::Node& n) {
    Job j;
    j.id   = id;
    j.name = str(n["name"]);
    if (j.name.empty()) j.name = id;

    j.runs_on = parseStringSeq(n["runs-on"]);
    j.if_condition = str(n["if"]);
    j.needs  = parseStringSeq(n["needs"]);
    j.env    = parseStringMap(n["env"]);
    j.timeout_minutes = integer(n["timeout-minutes"]);
    j.continue_on_error = boolean(n["continue-on-error"]);
    j.outputs = parseStringMap(n["outputs"]);

    // defaults:
    if (n["defaults"] && n["defaults"]["run"]) {
        j.default_shell       = str(n["defaults"]["run"]["shell"]);
        j.default_working_dir = str(n["defaults"]["run"]["working-directory"]);
    }

    // matrix strategy:
    if (n["strategy"]) {
        const auto& strat = n["strategy"];
        if (strat["matrix"]) {
            j.matrix = parseMatrix(strat["matrix"]);
        }
        // fail-fast: default true per GitHub Actions spec
        if (strat["fail-fast"] && strat["fail-fast"].IsScalar()) {
            j.fail_fast = strat["fail-fast"].as<bool>();
        }
        // max-parallel: 0 = unlimited
        if (strat["max-parallel"] && strat["max-parallel"].IsScalar()) {
            j.max_parallel = strat["max-parallel"].as<int>();
        }
    }

    // steps:
    if (n["steps"] && n["steps"].IsSequence()) {
        int idx = 0;
        for (const auto& sn : n["steps"]) {
            j.steps.push_back(parseStep(sn, idx++));
        }
    }

    return j;
}

// ─── parseTrigger ─────────────────────────────────────────────────────────

/// Parse a single event-filter node (e.g. the value under "push:" key).
/// The node may be null/scalar (no filters) or a mapping with branches:/tags:/etc.
static TriggerFilter parseTriggerFilter(const YAML::Node& n) {
    TriggerFilter tf;
    if (!n || n.IsNull() || n.IsScalar()) return tf; // no filters
    if (!n.IsMap()) return tf;

    if (n["branches"])        tf.branches        = parseStringSeq(n["branches"]);
    if (n["branches-ignore"]) tf.branches_ignore = parseStringSeq(n["branches-ignore"]);
    if (n["tags"])            tf.tags            = parseStringSeq(n["tags"]);
    if (n["tags-ignore"])     tf.tags_ignore     = parseStringSeq(n["tags-ignore"]);
    if (n["paths"])           tf.paths           = parseStringSeq(n["paths"]);
    if (n["paths-ignore"])    tf.paths_ignore    = parseStringSeq(n["paths-ignore"]);
    if (n["types"])           tf.types           = parseStringSeq(n["types"]);

    return tf;
}

// ─── parseWorkflow ────────────────────────────────────────────────────────

Workflow parseWorkflow(const std::string& yaml_content) {
    YAML::Node root;
    try {
        root = YAML::Load(yaml_content);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error(std::string("Workflow YAML parse error: ") + e.what());
    }

    Workflow wf;
    wf.name = str(root["name"]);
    wf.env  = parseStringMap(root["env"]);

    // ── Parse 'on:' trigger block ───────────────────────────────────────
    // The 'on:' key can be:
    //   on: push                             → scalar event name
    //   on: [push, pull_request]             → sequence of event names
    //   on:                                  → mapping of event→filter
    //     push:
    //       branches: [main]
    //     pull_request:
    //       types: [opened, synchronize]
    if (root["on"]) {
        const auto& on_node = root["on"];
        if (on_node.IsScalar()) {
            // Single event name
            wf.triggers[on_node.as<std::string>()] = TriggerFilter{};
        } else if (on_node.IsSequence()) {
            // List of event names, no filters
            for (const auto& ev : on_node) {
                wf.triggers[ev.as<std::string>()] = TriggerFilter{};
            }
        } else if (on_node.IsMap()) {
            // Map of event → filter config
            for (const auto& kv : on_node) {
                std::string event_name = kv.first.as<std::string>();
                wf.triggers[event_name] = parseTriggerFilter(kv.second);
            }
        }
    }

    if (root["defaults"] && root["defaults"]["run"]) {
        wf.default_shell       = str(root["defaults"]["run"]["shell"]);
        wf.default_working_dir = str(root["defaults"]["run"]["working-directory"]);
    }

    if (root["concurrency"]) {
        if (root["concurrency"].IsScalar()) {
            wf.concurrency = str(root["concurrency"]);
        } else if (root["concurrency"].IsMap()) {
            wf.concurrency = str(root["concurrency"]["group"]);
            wf.concurrency_cancel_in_progress =
                boolean(root["concurrency"]["cancel-in-progress"]);
        }
    }

    if (root["jobs"] && root["jobs"].IsMap()) {
        for (const auto& kv : root["jobs"]) {
            std::string id = kv.first.as<std::string>();
            wf.jobs[id] = parseJob(id, kv.second);
        }
    }

    return wf;
}

// ─── parseWorkflowFile ────────────────────────────────────────────────────

Workflow parseWorkflowFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open workflow file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return parseWorkflow(ss.str());
}

// ─── validateWorkflow ─────────────────────────────────────────────────────

std::vector<std::string> validateWorkflow(const Workflow& wf) {
    std::vector<std::string> errors;

    if (wf.jobs.empty()) {
        errors.push_back("Workflow has no jobs defined");
    }

    for (auto& [id, job] : wf.jobs) {
        if (job.runs_on.empty()) {
            errors.push_back("Job '" + id + "' has no runs-on");
        }
        if (job.steps.empty()) {
            errors.push_back("Job '" + id + "' has no steps");
        }
        for (auto& step : job.steps) {
            if (step.run.empty() && step.uses.empty()) {
                errors.push_back("Step '" + step.id + "' in job '" + id
                                 + "' has neither 'run' nor 'uses'");
            }
        }
        // Check needs references exist
        for (auto& need : job.needs) {
            if (wf.jobs.find(need) == wf.jobs.end()) {
                errors.push_back("Job '" + id + "' needs '" + need
                                 + "' which is not defined");
            }
        }
    }

    return errors;
}

} // namespace runner
