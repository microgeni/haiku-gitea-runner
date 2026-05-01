#pragma once
// WorkflowParser.h — GitHub Actions workflow YAML parser
//
// Parses a workflow file (or the workflow_payload bytes from a Task) into
// an in-memory representation that the TaskExecutor can run.
//
// We only parse fields relevant to the host-executor (no Docker).
// Expression evaluation (${{ }}) is deferred to ExprEvaluator.

#include <string>
#include <vector>
#include <map>
#include <optional>

namespace runner {

// ─── Step ─────────────────────────────────────────────────────────────────

/// A single step within a job.
struct Step {
    std::string id;           ///< explicit id or generated from index
    std::string name;         ///< display name (may contain ${{ }})
    std::string uses;         ///< action reference, e.g. "actions/checkout@v4"
    std::string run;          ///< inline shell script
    std::string shell;        ///< "sh" | "bash" | "pwsh" (default: "sh" on Haiku)
    std::string working_dir;  ///< step-level working-directory override

    // Condition: evaluated before running the step
    std::string if_condition; ///< raw ${{ }} expression string

    // Environment variables for this step
    std::map<std::string,std::string> env;

    // For 'uses:' steps — input parameters
    std::map<std::string,std::string> with;

    // Continue even if this step fails?
    bool continue_on_error = false;

    // Timeout for this individual step (minutes, 0 = inherit job timeout)
    int timeout_minutes = 0;
};

// ─── Job ──────────────────────────────────────────────────────────────────

/// Matrix configuration for a job.
struct Matrix {
    // Each key maps to a list of values.
    // e.g. { "os": ["ubuntu", "haiku"], "arch": ["x64", "arm64"] }
    std::map<std::string, std::vector<std::string>> axes;

    // Explicit include/exclude entries (map of key→value)
    std::vector<std::map<std::string,std::string>> include;
    std::vector<std::map<std::string,std::string>> exclude;

    bool empty() const { return axes.empty() && include.empty(); }
};

/// A single job within the workflow.
struct Job {
    std::string id;           ///< job identifier key in the YAML
    std::string name;         ///< display name
    std::vector<std::string> runs_on; ///< runner labels
    std::string if_condition; ///< job-level condition
    std::vector<std::string> needs;   ///< dependency job IDs

    // Job-level environment variables
    std::map<std::string,std::string> env;

    // Default settings
    std::string default_shell;
    std::string default_working_dir;

    // Steps (in order)
    std::vector<Step> steps;

    // Matrix (if present)
    std::optional<Matrix> matrix;

    // Job-level timeout (minutes, 0 = use workflow default = 360)
    int timeout_minutes = 0;

    // Continue if job fails (for matrix)
    bool continue_on_error = false;

    // strategy: block
    bool fail_fast    = true;  ///< Abort remaining wave jobs on first failure (default: true)
    int  max_parallel = 0;     ///< Max concurrent jobs in wave; 0 = unlimited

    // Outputs exported to dependent jobs
    std::map<std::string,std::string> outputs;  // name → expression
};

// ─── Workflow ─────────────────────────────────────────────────────────────

/// Top-level workflow document.
struct Workflow {
    std::string name;       ///< workflow display name

    // Global environment variables (lowest precedence)
    std::map<std::string,std::string> env;

    // Default settings applied to all jobs unless overridden
    std::string default_shell;
    std::string default_working_dir;

    // Jobs (key = job ID)
    std::map<std::string, Job> jobs;

    // Concurrency group string (raw, may contain ${{ }})
    std::string concurrency;
    bool concurrency_cancel_in_progress = false;
};

// ─── Parser ───────────────────────────────────────────────────────────────

/// Parse a workflow from YAML bytes.
/// @throws std::runtime_error on parse error
Workflow parseWorkflow(const std::string& yaml_content);

/// Parse a workflow from a file path.
/// @throws std::runtime_error on file read or parse error
Workflow parseWorkflowFile(const std::string& path);

/// Validate a parsed workflow for obvious issues.
/// Returns a list of error messages (empty = valid).
std::vector<std::string> validateWorkflow(const Workflow& wf);

} // namespace runner
