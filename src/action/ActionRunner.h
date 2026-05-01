#pragma once
// ActionRunner.h — Support for 'uses:' steps (GitHub Actions)
//
// Handles three forms of action references:
//   uses: owner/repo@ref          — remote action (downloaded via git/zip)
//   uses: ./path/to/local-action  — local action relative to workspace
//   uses: docker://image:tag       — Docker action (NOT supported on Haiku)
//
// For remote actions, we first check a local cache directory, then
// download via git clone or HTTP zip (whichever is available).
//
// Action types (determined by action.yml / action.yaml):
//   composite   — has runs.steps[] — recursively execute as steps
//   javascript  — runs Node.js script (requires node on PATH)
//   docker      — not supported on Haiku (emit warning, skip)

#include "../workflow/WorkflowParser.h"
#include "../workflow/ExprEvaluator.h"
#include "../process/EnvManager.h"
#include "../runner/LogForwarder.h"

#include <string>
#include <map>
#include <vector>
#include <optional>
#include <filesystem>

namespace runner {

// ─── action.yml schema ────────────────────────────────────────────────────

enum class ActionType {
    Composite,
    JavaScript,
    Docker,
    Unknown,
};

struct ActionInput {
    std::string name;
    std::string description;
    std::string default_value;
    bool required = false;
};

struct ActionOutput {
    std::string name;
    std::string description;
    std::string value;  // expression, for composite actions
};

/// Parsed action.yml / action.yaml
struct ActionDefinition {
    std::string name;
    std::string description;
    std::string author;

    std::map<std::string, ActionInput>  inputs;
    std::map<std::string, ActionOutput> outputs;

    ActionType  type = ActionType::Unknown;

    // For composite actions
    std::vector<Step> steps;  // re-uses WorkflowParser::Step

    // For javascript actions
    std::string main_script;   // runs.main
    std::string pre_script;    // runs.pre (optional)
    std::string post_script;   // runs.post (optional)
    std::string using_runtime; // "node16", "node20" etc.

    // For docker actions
    std::string docker_image;
};

/// A deferred post: script from a JavaScript action.
/// Collected into a queue during step execution and drained in reverse
/// order after all steps complete, matching GitHub Actions semantics.
struct PostScript {
    std::string node_bin;          ///< path to the node executable
    std::string script_path;       ///< absolute path to the post: .js file
    std::string working_dir;       ///< cwd for the script (job workspace)
    std::vector<std::string> env;  ///< flat "KEY=VALUE" environment
};

/// Result of running an action step.
struct ActionRunResult {
    bool success = false;
    int  exit_code = 0;
    std::map<std::string,std::string> outputs;
};

// ─── ActionCache ──────────────────────────────────────────────────────────

/// Manages the local cache of downloaded actions.
/// Cache root: $RUNNER_TOOL_CACHE/actions or /tmp/act_runner_actions/
class ActionCache {
public:
    explicit ActionCache(std::filesystem::path cache_root);

    /// Return the local path for an action, or empty if not cached.
    std::filesystem::path cachedPath(const std::string& owner,
                                      const std::string& repo,
                                      const std::string& ref) const;

    /// Download and cache an action. Returns the cache path.
    /// @throws std::runtime_error on download failure
    std::filesystem::path fetchAction(const std::string& owner,
                                       const std::string& repo,
                                       const std::string& ref,
                                       const std::string& gitea_url);

private:
    std::filesystem::path cache_root_;

    bool downloadViaGit(const std::string& url,
                         const std::filesystem::path& dest) const;
    bool downloadViaZip(const std::string& url,
                         const std::filesystem::path& dest) const;
};

// ─── ActionRunner ─────────────────────────────────────────────────────────

/// Resolves and executes 'uses:' action steps.
class ActionRunner {
public:
    /// @param workspace_dir   job workspace root
    /// @param cache           action cache (shared across steps in a job)
    /// @param default_shell   shell to use for composite action run: steps
    /// @param gitea_url       Gitea server URL (for fetching actions)
    ActionRunner(std::filesystem::path workspace_dir,
                 ActionCache&          cache,
                 std::string           default_shell,
                 std::string           gitea_url);

    /// Execute a 'uses:' step.
    /// If the action has a post: script, it is appended to *post_queue
    /// (if non-null) for deferred execution after all steps complete.
    ActionRunResult run(
        const Step&           step,         // the uses: step
        const ExprContext&    expr_ctx,
        EnvManager&           env_mgr,
        LogForwarder&         log,
        const std::vector<std::pair<std::string,std::string>>& base_env,
        std::vector<PostScript>* post_queue = nullptr  // deferred post: scripts
    );

    /// Parse an action.yml file into an ActionDefinition.
    static ActionDefinition parseActionYaml(const std::filesystem::path& path);

    /// Parse  owner/repo@ref  or  owner/repo/subdir@ref  into components.
    /// Returns false if the reference is malformed.
    static bool parseActionRef(const std::string& uses,
                                std::string& owner,
                                std::string& repo,
                                std::string& ref,
                                std::string& subdir);

private:
    std::filesystem::path workspace_dir_;
    ActionCache&          cache_;
    std::string           default_shell_;
    std::string           gitea_url_;

    /// Resolve a 'uses:' reference to a local directory containing action.yml
    std::filesystem::path resolveActionDir(
        const std::string& uses,
        const std::string& gitea_runtime_token = "");

    ActionRunResult runComposite(
        const ActionDefinition& def,
        const Step&             step,
        const ExprContext&      expr_ctx,
        EnvManager&             env_mgr,
        LogForwarder&           log,
        const std::vector<std::pair<std::string,std::string>>& base_env,
        int                     recursion_depth = 0
    );

    /// Internal: resolve + execute a nested uses: from inside a composite action.
    ActionRunResult runRecursive(
        const Step&             step,
        const ExprContext&      expr_ctx,
        EnvManager&             env_mgr,
        LogForwarder&           log,
        const std::vector<std::pair<std::string,std::string>>& base_env,
        int                     recursion_depth
    );

    ActionRunResult runJavaScript(
        const ActionDefinition& def,
        const Step&             step,
        const ExprContext&      expr_ctx,
        EnvManager&             env_mgr,
        LogForwarder&           log,
        const std::vector<std::pair<std::string,std::string>>& base_env,
        const std::filesystem::path& action_dir,
        std::vector<PostScript>* post_queue = nullptr
    );
};

} // namespace runner
