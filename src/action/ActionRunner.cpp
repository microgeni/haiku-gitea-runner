// ActionRunner.cpp — 'uses:' action resolution and execution
#include "ActionRunner.h"

#include "../process/ProcessSpawner.h"
#include "../util/Logger.h"

#include <yaml-cpp/yaml.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cstdlib>

namespace runner {

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════════
// ActionCache
// ═══════════════════════════════════════════════════════════════════════════

ActionCache::ActionCache(fs::path cache_root)
    : cache_root_(std::move(cache_root))
{
    fs::create_directories(cache_root_);
}

fs::path ActionCache::cachedPath(const std::string& owner,
                                  const std::string& repo,
                                  const std::string& ref) const
{
    fs::path p = cache_root_ / owner / repo / ref;
    if (fs::exists(p / "action.yml") || fs::exists(p / "action.yaml")) {
        return p;
    }
    return {};
}

bool ActionCache::downloadViaGit(const std::string& url,
                                  const fs::path& dest) const
{
    // Check if git is available
    if (system("git --version >/dev/null 2>&1") != 0) return false;

    fs::create_directories(dest);
    std::string cmd = "git clone --depth 1 "
                    + url + " " + dest.string()
                    + " >/dev/null 2>&1";
    return system(cmd.c_str()) == 0;
}

bool ActionCache::downloadViaZip(const std::string& url,
                                  const fs::path& dest) const
{
    // Use curl + unzip if git is unavailable
    if (system("curl --version >/dev/null 2>&1") != 0) return false;

    std::string zip_path = (cache_root_ / "tmp_action.zip").string();
    std::string cmd = "curl -sSL '" + url + "' -o '" + zip_path + "'";
    if (system(cmd.c_str()) != 0) return false;

    fs::create_directories(dest);
    cmd = "unzip -q '" + zip_path + "' -d '" + dest.string() + "' 2>/dev/null";
    int ret = system(cmd.c_str());
    fs::remove(zip_path);

    if (ret != 0) return false;

    // Unzip often creates a single subdirectory — flatten it
    for (auto& entry : fs::directory_iterator(dest)) {
        if (fs::is_directory(entry)) {
            // Move contents up one level
            for (auto& sub : fs::directory_iterator(entry)) {
                fs::rename(sub.path(), dest / sub.path().filename());
            }
            fs::remove(entry.path());
            break;
        }
    }
    return true;
}

fs::path ActionCache::fetchAction(const std::string& owner,
                                   const std::string& repo,
                                   const std::string& ref,
                                   const std::string& gitea_url)
{
    fs::path dest = cache_root_ / owner / repo / ref;

    // Already cached?
    auto cached = cachedPath(owner, repo, ref);
    if (!cached.empty()) return cached;

    LOG_INFO("ActionCache", "Fetching action " << owner << "/" << repo << "@" << ref);

    // Try git clone from Gitea
    std::string git_url = gitea_url + "/" + owner + "/" + repo;
    // Append @ref as a branch/tag
    std::string clone_url = git_url;

    // Try with --branch for the ref
    fs::create_directories(dest.parent_path());
    std::string cmd = "git clone --depth 1 --branch '" + ref + "' '"
                    + clone_url + "' '" + dest.string() + "' >/dev/null 2>&1";
    if (system(cmd.c_str()) == 0) {
        LOG_INFO("ActionCache", "Cloned " << owner << "/" << repo << "@" << ref);
        return dest;
    }

    // Fallback: try zip download from Gitea archive endpoint
    std::string zip_url = gitea_url + "/" + owner + "/" + repo
                        + "/archive/" + ref + ".zip";
    if (downloadViaZip(zip_url, dest)) {
        LOG_INFO("ActionCache", "Downloaded (zip) " << owner << "/" << repo << "@" << ref);
        return dest;
    }

    throw std::runtime_error(
        "Failed to fetch action " + owner + "/" + repo + "@" + ref
        + " — check that the action exists on " + gitea_url);
}

// ═══════════════════════════════════════════════════════════════════════════
// ActionDefinition parsing
// ═══════════════════════════════════════════════════════════════════════════

static std::string strNode(const YAML::Node& n) {
    if (!n || n.IsNull()) return "";
    return n.as<std::string>("");
}

ActionDefinition ActionRunner::parseActionYaml(const fs::path& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path.string());
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Failed to parse action.yml at '"
                                 + path.string() + "': " + e.what());
    }

    ActionDefinition def;
    def.name        = strNode(root["name"]);
    def.description = strNode(root["description"]);
    def.author      = strNode(root["author"]);

    // Inputs
    if (root["inputs"] && root["inputs"].IsMap()) {
        for (const auto& kv : root["inputs"]) {
            ActionInput inp;
            inp.name          = kv.first.as<std::string>();
            inp.description   = strNode(kv.second["description"]);
            inp.default_value = strNode(kv.second["default"]);
            inp.required      = kv.second["required"]
                              ? kv.second["required"].as<bool>(false) : false;
            def.inputs[inp.name] = inp;
        }
    }

    // Outputs
    if (root["outputs"] && root["outputs"].IsMap()) {
        for (const auto& kv : root["outputs"]) {
            ActionOutput out;
            out.name        = kv.first.as<std::string>();
            out.description = strNode(kv.second["description"]);
            out.value       = strNode(kv.second["value"]);
            def.outputs[out.name] = out;
        }
    }

    // runs:
    if (root["runs"]) {
        const auto& runs = root["runs"];
        std::string using_ = strNode(runs["using"]);

        if (using_ == "composite") {
            def.type = ActionType::Composite;
            // Parse steps (same schema as workflow steps)
            if (runs["steps"] && runs["steps"].IsSequence()) {
                int idx = 0;
                for (const auto& sn : runs["steps"]) {
                    Step s;
                    s.id               = strNode(sn["id"]);
                    if (s.id.empty())  s.id = "step_" + std::to_string(idx);
                    s.name             = strNode(sn["name"]);
                    s.uses             = strNode(sn["uses"]);
                    s.run              = strNode(sn["run"]);
                    s.shell            = strNode(sn["shell"]);
                    s.working_dir      = strNode(sn["working-directory"]);
                    s.if_condition     = strNode(sn["if"]);
                    if (sn["continue-on-error"])
                        s.continue_on_error = sn["continue-on-error"].as<bool>(false);
                    if (sn["env"] && sn["env"].IsMap()) {
                        for (const auto& ev : sn["env"]) {
                            s.env[ev.first.as<std::string>()] = strNode(ev.second);
                        }
                    }
                    if (sn["with"] && sn["with"].IsMap()) {
                        for (const auto& wv : sn["with"]) {
                            s.with[wv.first.as<std::string>()] = strNode(wv.second);
                        }
                    }
                    def.steps.push_back(std::move(s));
                    ++idx;
                }
            }
        } else if (using_.substr(0, 4) == "node") {
            def.type           = ActionType::JavaScript;
            def.using_runtime  = using_;
            def.main_script    = strNode(runs["main"]);
            def.pre_script     = strNode(runs["pre"]);
            def.post_script    = strNode(runs["post"]);
        } else if (using_ == "docker") {
            def.type         = ActionType::Docker;
            def.docker_image = strNode(runs["image"]);
        }
    }

    return def;
}

// ═══════════════════════════════════════════════════════════════════════════
// ActionRunner
// ═══════════════════════════════════════════════════════════════════════════

ActionRunner::ActionRunner(fs::path workspace_dir,
                             ActionCache& cache,
                             std::string default_shell,
                             std::string gitea_url)
    : workspace_dir_(std::move(workspace_dir))
    , cache_(cache)
    , default_shell_(std::move(default_shell))
    , gitea_url_(std::move(gitea_url))
{}

// ─── parseActionRef ───────────────────────────────────────────────────────

bool ActionRunner::parseActionRef(const std::string& uses,
                                   std::string& owner,
                                   std::string& repo,
                                   std::string& ref,
                                   std::string& subdir)
{
    // Format: owner/repo@ref  or  owner/repo/subdir@ref
    auto at = uses.find('@');
    if (at == std::string::npos) return false;

    ref = uses.substr(at + 1);
    std::string path = uses.substr(0, at);

    auto slash1 = path.find('/');
    if (slash1 == std::string::npos) return false;

    owner = path.substr(0, slash1);
    std::string rest = path.substr(slash1 + 1);

    auto slash2 = rest.find('/');
    if (slash2 == std::string::npos) {
        repo   = rest;
        subdir = "";
    } else {
        repo   = rest.substr(0, slash2);
        subdir = rest.substr(slash2 + 1);
    }
    return !owner.empty() && !repo.empty() && !ref.empty();
}

// ─── resolveActionDir ─────────────────────────────────────────────────────

fs::path ActionRunner::resolveActionDir(const std::string& uses,
                                         const std::string& /*runtime_token*/)
{
    // Local action: starts with "./"
    if (uses.substr(0, 2) == "./") {
        fs::path local = workspace_dir_ / uses.substr(2);
        if (!fs::exists(local / "action.yml") &&
            !fs::exists(local / "action.yaml")) {
            throw std::runtime_error(
                "Local action not found at: " + local.string());
        }
        return local;
    }

    // Docker action: not supported
    if (uses.substr(0, 9) == "docker://") {
        throw std::runtime_error(
            "Docker actions are not supported on Haiku: " + uses);
    }

    // Remote: owner/repo@ref
    std::string owner, repo, ref, subdir;
    if (!parseActionRef(uses, owner, repo, ref, subdir)) {
        throw std::runtime_error("Invalid action reference: " + uses);
    }

    fs::path action_dir = cache_.fetchAction(owner, repo, ref, gitea_url_);
    if (!subdir.empty()) action_dir /= subdir;

    if (!fs::exists(action_dir / "action.yml") &&
        !fs::exists(action_dir / "action.yaml")) {
        throw std::runtime_error(
            "action.yml not found in " + action_dir.string());
    }
    return action_dir;
}

// ─── run ──────────────────────────────────────────────────────────────────

// ── Built-in action stubs ────────────────────────────────────────────────
// These are well-known actions that are either GitHub-infrastructure actions
// (checkout, cache, upload-artifact, download-artifact) or Docker-only actions
// that we handle specially in the host-executor model.
//
// Matching is done on the base name, ignoring @version suffix so that
// actions/checkout@v1, @v2, @v3, @v4 all match.

static std::string actionBaseName(const std::string& uses) {
    // Strip @ref suffix: "actions/checkout@v4" → "actions/checkout"
    auto at = uses.find('@');
    if (at != std::string::npos) return uses.substr(0, at);
    return uses;
}

ActionRunResult ActionRunner::run(
    const Step&           step,
    const ExprContext&    expr_ctx,
    EnvManager&           env_mgr,
    LogForwarder&         log,
    const std::vector<std::pair<std::string,std::string>>& base_env,
    std::vector<PostScript>* post_queue)
{
    LOG_INFO("ActionRunner", "Resolving action: " << step.uses);

    const std::string base = actionBaseName(step.uses);
    ExprEvaluator eval(expr_ctx);

    // ── actions/checkout ─────────────────────────────────────────────────
    // Clone the repository into the workspace (or a subdirectory).
    //
    // Supported with: inputs:
    //   repository   — owner/repo  (default: github.repository)
    //   ref          — branch/tag/SHA  (default: github.ref or github.sha)
    //   path         — relative path within workspace  (default: ".")
    //   token        — PAT for private repos  (default: github.token)
    //   depth        — fetch-depth (0 = full clone; default: 1)
    //   submodules   — bool (default: false)
    //
    // On Haiku (host executor) we perform a real git clone.  If git is not
    // found we emit a warning and succeed so that workflows that don't
    // actually need the source (e.g. matrix checks) keep working.
    if (base == "actions/checkout") {
        // Resolve inputs
        auto withVal = [&](const std::string& key, const std::string& def_val) {
            auto it = step.with.find(key);
            if (it != step.with.end()) return eval.interpolate(it->second);
            return def_val;
        };

        std::string repo  = withVal("repository",
            eval.interpolate("${{ github.repository }}"));
        std::string ref   = withVal("ref",
            eval.interpolate("${{ github.ref }}"));
        std::string path  = withVal("path", "");
        std::string token = withVal("token",
            eval.interpolate("${{ github.token }}"));
        std::string depth_str = withVal("fetch-depth", "1");
        std::string submodules = withVal("submodules", "false");

        // Determine clone destination inside workspace
        fs::path dest = workspace_dir_;
        if (!path.empty()) dest /= path;

        // Determine git server URL
        std::string server_url = eval.interpolate("${{ github.server_url }}");
        if (server_url.empty()) server_url = gitea_url_;
        if (server_url.empty()) server_url = "https://github.com";
        // Strip trailing slash
        while (!server_url.empty() && server_url.back() == '/') server_url.pop_back();

        // Build clone URL: embed token for private repo access
        // Format: https://<token>@<host>/<owner>/<repo>.git
        std::string clone_url;
        {
            // Parse scheme from server_url
            auto scheme_end = server_url.find("://");
            std::string scheme = "https";
            std::string host_path = server_url;
            if (scheme_end != std::string::npos) {
                scheme    = server_url.substr(0, scheme_end);
                host_path = server_url.substr(scheme_end + 3);
            }
            if (!token.empty() && token != "false" && token != "0") {
                clone_url = scheme + "://oauth2:" + token
                          + "@" + host_path + "/" + repo + ".git";
            } else {
                clone_url = server_url + "/" + repo + ".git";
            }
        }

        // Normalise ref: "refs/heads/main" → "main", "refs/tags/v1" → "v1"
        std::string clone_ref = ref;
        if (clone_ref.substr(0, 11) == "refs/heads/") clone_ref = clone_ref.substr(11);
        else if (clone_ref.substr(0, 10) == "refs/tags/") clone_ref = clone_ref.substr(10);

        int fetch_depth = 1;
        try { fetch_depth = std::stoi(depth_str); } catch (...) {}

        // Build git command
        // Use -c http.extraHeader to pass auth rather than embedding in URL
        // for better security; but embedded token is simpler for Haiku.
        std::string git_cmd = "git clone";
        if (fetch_depth > 0) git_cmd += " --depth=" + std::to_string(fetch_depth);
        if (!clone_ref.empty()) git_cmd += " --branch=" + clone_ref;
        git_cmd += " -- " + clone_url + " " + dest.string();

        if (submodules == "true" || submodules == "recursive") {
            // Will be handled after clone via git submodule update
        }

        log.append("##[group]Checkout " + repo
                   + (clone_ref.empty() ? "" : " @ " + clone_ref));
        log.append("Cloning " + repo + " into " + dest.string());

        // Check git availability first.
        // Build an environment from the current process (which includes any
        // PATH restriction set by the caller/job), plus suppress prompts.
        std::vector<std::pair<std::string,std::string>> git_env_overrides;
        git_env_overrides.emplace_back("GIT_TERMINAL_PROMPT", "0");
        git_env_overrides.emplace_back("GCM_INTERACTIVE",     "never");
        auto git_envp = buildEnv(git_env_overrides, {});

        ProcessSpawner git_check;
        // Use "git --version" rather than "command -v git" — on Haiku,
        // command -v returns 0 even when the binary is not in PATH.
        auto check_result = git_check.run("/bin/sh", "git --version",
                                          dest.string(), git_envp,
                                          nullptr, /*timeout_s=*/5);
        if (check_result.exit_code != 0) {
            log.append("##[warning]'git' not found in PATH — skipping checkout."
                       " Install git with: pkgman install git");
            log.append("##[endgroup]");
            // Return success — allow workflows to proceed without source on
            // hosts that don't have git (e.g. minimal CI environments).
            return {true, 0, {}};
        }

        // Create destination directory
        try { fs::create_directories(dest); } catch (...) {}

        // Run git clone (reuse git_envp built above for the git check)
        ProcessSpawner spawner;

        bool clone_ok = false;
        {
            ProcessResult clone_result = spawner.run("/bin/sh", git_cmd,
                dest.string(), git_envp,
                [&log](const std::string& line, bool is_err) {
                    if (is_err) log.append("##[debug]" + line);
                    else        log.append(line);
                }, /*timeout_s=*/300);
            clone_ok = (clone_result.exit_code == 0);
        }

        // Submodule support
        if (clone_ok && (submodules == "true" || submodules == "recursive")) {
            std::string sub_cmd = "git submodule update --init";
            if (submodules == "recursive") sub_cmd += " --recursive";
            ProcessSpawner sub_spawn;
            sub_spawn.run("/bin/sh", sub_cmd, dest.string(), git_envp,
                [&log](const std::string& line, bool is_err) {
                    if (is_err) log.append("##[debug]" + line);
                    else        log.append(line);
                }, /*timeout_s=*/300);
        }

        log.append("##[endgroup]");

        if (!clone_ok) {
            log.append("##[error]git clone failed for " + repo);
            return {step.continue_on_error, step.continue_on_error ? 0 : 1, {}};
        }

        return {true, 0, {}};
    }

    // ── actions/cache ─────────────────────────────────────────────────────
    // The cache action communicates with the cache server via environment
    // variables (ACTIONS_CACHE_URL) and the HTTP cache API.  On Haiku, our
    // CacheServer implements this API locally, so the real actions/cache JS
    // action should work when node is available.
    //
    // When node is NOT available, we emit a warning and succeed (so workflows
    // using cache as an optimisation don't hard-fail).
    if (base == "actions/cache") {
        // Check for node
        ProcessSpawner node_check;
        auto check = node_check.run("/bin/sh", "command -v node || command -v node.js",
                                    workspace_dir_.string(), {});
        if (check.exit_code != 0) {
            log.append("##[warning]'node' not found — actions/cache requires Node.js."
                       " Install with: pkgman install nodejs20");
            log.append("##[warning]Cache step skipped (cache will be a miss).");
            return {true, 0, {}};  // non-fatal: treat as cache miss
        }
        // Fall through to normal JS action handling (fetch + run action.yml)
    }

    // ── actions/upload-artifact / actions/download-artifact ──────────────
    // Our CacheServer implements the Twirp artifact API, so the real JS actions
    // work when node is available.  Without node, emit a warning.
    if (base == "actions/upload-artifact" || base == "actions/download-artifact") {
        ProcessSpawner node_check;
        auto check = node_check.run("/bin/sh", "command -v node || command -v node.js",
                                    workspace_dir_.string(), {});
        if (check.exit_code != 0) {
            log.append("##[warning]'node' not found — " + base
                       + " requires Node.js. Install with: pkgman install nodejs20");
            log.append("##[warning]Artifact step skipped.");
            return {true, 0, {}};  // non-fatal
        }
        // Fall through to normal JS action handling
    }

    fs::path action_dir;
    try {
        action_dir = resolveActionDir(step.uses);
    } catch (const std::exception& e) {
        log.append(std::string("##[error]") + e.what());
        return {false, 1, {}};
    }

    // Parse action.yml (or action.yaml)
    fs::path yml_path = action_dir / "action.yml";
    if (!fs::exists(yml_path)) yml_path = action_dir / "action.yaml";

    ActionDefinition def;
    try {
        def = parseActionYaml(yml_path);
    } catch (const std::exception& e) {
        log.append(std::string("##[error]Failed to parse action definition: ") + e.what());
        return {false, 1, {}};
    }

    // Build inputs context: merge defaults + step.with values (evaluated)
    ExprContext action_ctx = expr_ctx;

    for (auto& [iname, idef] : def.inputs) {
        std::string value = idef.default_value;
        auto it = step.with.find(iname);
        if (it != step.with.end()) {
            value = eval.interpolate(it->second);
        } else if (idef.required && value.empty()) {
            log.append("##[error]Required input '" + iname + "' not provided for action " + step.uses);
        }
        action_ctx.setString("inputs." + iname, value);
    }

    switch (def.type) {
        case ActionType::Composite:
            return runComposite(def, step, action_ctx, env_mgr, log, base_env, /*depth=*/0);

        case ActionType::JavaScript:
            return runJavaScript(def, step, action_ctx, env_mgr, log, base_env,
                                 action_dir, post_queue);

        case ActionType::Docker:
            log.append("##[error]Docker actions are not supported on Haiku: " + step.uses);
            return {step.continue_on_error, step.continue_on_error ? 0 : 1, {}};

        case ActionType::Unknown:
        default:
            log.append("##[error]Unknown action type in " + yml_path.string());
            return {false, 1, {}};
    }
}

// ─── runRecursive ─────────────────────────────────────────────────────────
// Internal: resolve a nested uses: reference inside a composite action and
// execute it, passing the recursion depth counter so we can detect cycles.

ActionRunResult ActionRunner::runRecursive(
    const Step&             step,
    const ExprContext&      expr_ctx,
    EnvManager&             env_mgr,
    LogForwarder&           log,
    const std::vector<std::pair<std::string,std::string>>& base_env,
    int                     recursion_depth)
{
    // Resolve the action directory (same logic as run())
    fs::path action_dir;
    try {
        action_dir = resolveActionDir(step.uses);
    } catch (const std::exception& e) {
        log.append("##[error]Cannot resolve nested action '"
                   + step.uses + "': " + e.what());
        return {false, 1, {}};
    }

    fs::path yml_path = action_dir / "action.yml";
    if (!fs::exists(yml_path)) yml_path = action_dir / "action.yaml";
    if (!fs::exists(yml_path)) {
        log.append("##[error]Nested action '" + step.uses
                   + "' has no action.yml in " + action_dir.string());
        return {false, 1, {}};
    }

    ActionDefinition def;
    try {
        def = parseActionYaml(yml_path);
    } catch (const std::exception& e) {
        log.append("##[error]Cannot parse nested action '" + step.uses
                   + "': " + e.what());
        return {false, 1, {}};
    }

    // Build inputs context from with: + defaults
    ExprContext action_ctx = expr_ctx;
    ExprEvaluator eval(expr_ctx);
    for (auto& [iname, idef] : def.inputs) {
        std::string value = idef.default_value;
        auto it = step.with.find(iname);
        if (it != step.with.end()) {
            value = eval.interpolate(it->second);
        } else if (idef.required && value.empty()) {
            log.append("##[error]Required input '" + iname
                       + "' not provided for nested action " + step.uses);
        }
        action_ctx.setString("inputs." + iname, value);
    }

    switch (def.type) {
        case ActionType::Composite:
            return runComposite(def, step, action_ctx, env_mgr, log, base_env,
                                recursion_depth);

        case ActionType::JavaScript:
            return runJavaScript(def, step, action_ctx, env_mgr, log, base_env,
                                 action_dir);

        case ActionType::Docker:
            log.append("##[error]Docker actions not supported on Haiku: " + step.uses);
            return {step.continue_on_error, step.continue_on_error ? 0 : 1, {}};

        default:
            log.append("##[error]Unknown nested action type: " + step.uses);
            return {false, 1, {}};
    }
}

// ─── runComposite ─────────────────────────────────────────────────────────

ActionRunResult ActionRunner::runComposite(
    const ActionDefinition& def,
    const Step&             /*step*/,
    const ExprContext&      expr_ctx,
    EnvManager&             env_mgr,
    LogForwarder&           log,
    const std::vector<std::pair<std::string,std::string>>& base_env,
    int                     recursion_depth)
{
    // Guard against infinitely-recursive composite actions.
    static constexpr int kMaxRecursionDepth = 10;
    if (recursion_depth > kMaxRecursionDepth) {
        log.append("##[error]Composite action recursion depth exceeded ("
                   + std::to_string(kMaxRecursionDepth) + ") — aborting");
        return {false, 1, {}};
    }

    LOG_INFO("ActionRunner", "Running composite action: " << def.name
             << " (depth=" << recursion_depth << ")");

    ActionRunResult overall{true, 0, {}};
    ExprContext current_ctx = expr_ctx;

    for (const auto& step : def.steps) {
        ExprEvaluator eval(current_ctx);

        // Check condition
        if (!step.if_condition.empty()) {
            if (!eval.evaluateCondition(step.if_condition)) {
                log.append("##[debug]Composite step '" + step.id + "' skipped");
                continue;
            }
        }

        if (!step.run.empty()) {
            // Inline run step
            std::string script = eval.interpolate(step.run);
            std::string shell  = step.shell.empty() ? default_shell_ : step.shell;

            auto step_env_pairs = base_env;
            for (auto& [k, v] : step.env) {
                step_env_pairs.emplace_back(k, eval.interpolate(v));
            }
            // Include cumulative env changes from previous sub-steps
            auto mgr_env = env_mgr.currentEnv();
            step_env_pairs.insert(step_env_pairs.end(), mgr_env.begin(), mgr_env.end());
            auto flat_env = buildEnv(step_env_pairs);

            ProcessSpawner spawner;
            auto on_out = [&log](const std::string& line, bool /*is_stderr*/) {
                log.append(line);
            };
            std::string work_dir = step.working_dir.empty()
                                 ? workspace_dir_.string()
                                 : step.working_dir;
            auto proc = spawner.run(shell, script,
                                    work_dir,
                                    flat_env, on_out,
                                    step.timeout_minutes * 60);

            if (proc.exit_code != 0 && !step.continue_on_error) {
                overall.success   = false;
                overall.exit_code = proc.exit_code;
                // Still collect any outputs written before the failure
                auto step_outs = env_mgr.parseOutputs();
                overall.outputs.insert(step_outs.begin(), step_outs.end());
                env_mgr.applyEnvChanges();
                env_mgr.applyPathChanges();
                break;
            }

            env_mgr.applyEnvChanges();
            env_mgr.applyPathChanges();

            // Collect outputs written by this sub-step into overall.outputs.
            auto step_outs = env_mgr.parseOutputs();
            overall.outputs.insert(step_outs.begin(), step_outs.end());

        } else if (!step.uses.empty()) {
            // Nested uses: — recursively call ActionRunner::run().
            // Build a sub-step with the inherited context + with: inputs.
            LOG_INFO("ActionRunner", "Resolving nested uses: '" << step.uses
                     << "' (depth " << recursion_depth + 1 << ")");

            // Interpolate with: values using the current context.
            Step sub_step = step;  // copy — we'll keep uses/with/env/id as-is
            for (auto& [k, v] : sub_step.with) {
                sub_step.with[k] = eval.interpolate(v);
            }

            // Merge step.env into base_env for the sub-call
            auto sub_base = base_env;
            auto mgr_env = env_mgr.currentEnv();
            sub_base.insert(sub_base.end(), mgr_env.begin(), mgr_env.end());
            for (auto& [k, v] : step.env) {
                sub_base.emplace_back(k, eval.interpolate(v));
            }

            ActionRunResult sub_result = runRecursive(
                sub_step, current_ctx, env_mgr, log, sub_base,
                recursion_depth + 1);

            // Merge sub-action outputs into overall
            overall.outputs.insert(sub_result.outputs.begin(),
                                   sub_result.outputs.end());

            if (!sub_result.success && !step.continue_on_error) {
                overall.success   = false;
                overall.exit_code = sub_result.exit_code;
                break;
            }
        }
    }

    return overall;
}

// ─── runJavaScript ────────────────────────────────────────────────────────

ActionRunResult ActionRunner::runJavaScript(
    const ActionDefinition& def,
    const Step&             step,
    const ExprContext&      expr_ctx,
    EnvManager&             env_mgr,
    LogForwarder&           log,
    const std::vector<std::pair<std::string,std::string>>& base_env,
    const fs::path&         action_dir,
    std::vector<PostScript>* post_queue)
{
    // Find node executable
    std::string node_bin;
    for (auto& candidate : {"/bin/node", "/usr/bin/node",
                             "/boot/home/config/bin/node"}) {
        if (fs::exists(candidate)) { node_bin = candidate; break; }
    }
    // Also try PATH
    if (node_bin.empty()) {
        if (system("node --version >/dev/null 2>&1") == 0) node_bin = "node";
    }

    if (node_bin.empty()) {
        log.append("##[error]JavaScript actions require Node.js, which is not "
                   "installed. Install with: pkgman install nodejs");
        log.append("##[error]Action: " + step.uses);
        // Even when node is absent, enqueue post: so it can be attempted later
        // (in case node gets added to PATH between now and post: drain time).
        if (post_queue && !def.post_script.empty()) {
            fs::path post_path = action_dir / def.post_script;
            if (fs::exists(post_path)) {
                PostScript ps;
                ps.node_bin    = "node";  // best-effort; will fail gracefully
                ps.script_path = post_path.string();
                ps.working_dir = workspace_dir_.string();
                ps.env         = {};
                post_queue->push_back(std::move(ps));
            }
        }
        return {step.continue_on_error, step.continue_on_error ? 0 : 1, {}};
    }

    std::string main_path = (action_dir / def.main_script).string();
    if (!fs::exists(main_path)) {
        log.append("##[error]Action main script not found: " + main_path);
        return {false, 1, {}};
    }

    // Build environment: inputs → INPUT_<NAME> vars (GitHub Actions convention)
    auto step_env = base_env;
    ExprEvaluator eval(expr_ctx);

    for (auto& [iname, idef] : def.inputs) {
        std::string upper = iname;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        std::string val = idef.default_value;
        auto it = step.with.find(iname);
        if (it != step.with.end()) val = eval.interpolate(it->second);
        step_env.emplace_back("INPUT_" + upper, val);
    }

    auto mgr_env = env_mgr.currentEnv();
    step_env.insert(step_env.end(), mgr_env.begin(), mgr_env.end());
    auto flat_env = buildEnv(step_env);

    ProcessSpawner spawner;
    auto on_out = [&log](const std::string& line, bool /*is_stderr*/) {
        log.append(line);
    };

    auto proc = spawner.run(node_bin, main_path,
                             workspace_dir_.string(),
                             flat_env, on_out,
                             step.timeout_minutes * 60);

    env_mgr.applyEnvChanges();
    env_mgr.applyPathChanges();
    auto outputs = env_mgr.parseOutputs();
    env_mgr.resetBetweenSteps();

    // Enqueue post: script for deferred execution after all steps finish.
    // post: runs even if main: failed (GitHub Actions guarantee).
    if (post_queue && !def.post_script.empty()) {
        fs::path post_path = action_dir / def.post_script;
        if (fs::exists(post_path)) {
            PostScript ps;
            ps.node_bin    = node_bin;
            ps.script_path = post_path.string();
            ps.working_dir = workspace_dir_.string();
            ps.env         = flat_env;
            post_queue->push_back(std::move(ps));
            LOG_DEBUG("ActionRunner", "Queued post: script for deferred run: "
                      << post_path.string());
        } else {
            LOG_DEBUG("ActionRunner", "post: script declared but not found: "
                      << post_path.string());
        }
    }

    return {proc.exit_code == 0, proc.exit_code, std::move(outputs)};
}

} // namespace runner
