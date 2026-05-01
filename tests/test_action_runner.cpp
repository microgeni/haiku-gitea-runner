// tests/test_action_runner.cpp — Unit tests for ActionRunner (static parsing + dispatch logic)
//
// These tests do NOT require network access.  They test:
//   1. ActionRunner::parseActionRef  (static, exposed via derived test fixture)
//   2. ActionRunner::parseActionYaml (reads action.yml from a temp file)
//   3. ActionCache construction + cachedPath() logic
//   4. ActionType detection
//   5. ActionInput / ActionOutput parsing

#include "test_runner.h"
#include "../src/action/ActionRunner.h"
#include "MockRunnerClient.h"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <sys/stat.h>  // chmod

using namespace test;
using namespace runner;
namespace fs = std::filesystem;

// ── Helpers ──────────────────────────────────────────────────────────────────

// Create a temp directory and write action.yml into it.
static fs::path makeTempActionDir(const std::string& yml_content) {
    fs::path dir = fs::temp_directory_path() / "act_runner_test_action";
    fs::create_directories(dir);
    std::ofstream f(dir / "action.yml");
    f << yml_content;
    return dir;
}

static void removeTempActionDir() {
    fs::remove_all(fs::temp_directory_path() / "act_runner_test_action");
}

// Subclass ActionRunner to expose parseActionRef (now public static)
// No longer needed — use ActionRunner directly
class TestableActionRunner {
public:
    // Just a namespace alias for clarity
    static bool parseActionRef(const std::string& uses,
                                std::string& owner, std::string& repo,
                                std::string& ref,   std::string& subdir) {
        return ActionRunner::parseActionRef(uses, owner, repo, ref, subdir);
    }
};

int main() {
    std::cout << "=== ActionRunner tests ===\n\n";

    // ── ActionCache construction ───────────────────────────────────────────

    run("ActionCache creates cache_root if it does not exist", []() {
        fs::path cache_dir = fs::temp_directory_path() / "act_runner_test_cache";
        fs::remove_all(cache_dir);  // start clean
        {
            ActionCache cache(cache_dir);
        }
        ASSERT(fs::exists(cache_dir));
        fs::remove_all(cache_dir);
    });

    run("ActionCache::cachedPath returns empty when action not cached", []() {
        fs::path cache_dir = fs::temp_directory_path() / "act_runner_test_cache2";
        ActionCache cache(cache_dir);
        auto p = cache.cachedPath("actions", "checkout", "v4");
        ASSERT(p.empty());
        fs::remove_all(cache_dir);
    });

    run("ActionCache::cachedPath returns path when action.yml present", []() {
        fs::path cache_dir = fs::temp_directory_path() / "act_runner_test_cache3";
        ActionCache cache(cache_dir);
        // Manually create the cache entry
        fs::path action_dir = cache_dir / "actions" / "checkout" / "v4";
        fs::create_directories(action_dir);
        std::ofstream f(action_dir / "action.yml");
        f << "name: checkout\nruns:\n  using: composite\n  steps: []\n";
        auto p = cache.cachedPath("actions", "checkout", "v4");
        ASSERT(!p.empty());
        ASSERT(p == action_dir);
        fs::remove_all(cache_dir);
    });

    run("ActionCache::cachedPath accepts action.yaml as well as action.yml", []() {
        fs::path cache_dir = fs::temp_directory_path() / "act_runner_test_cache4";
        ActionCache cache(cache_dir);
        fs::path action_dir = cache_dir / "owner" / "repo" / "main";
        fs::create_directories(action_dir);
        std::ofstream f(action_dir / "action.yaml");
        f << "name: test\nruns:\n  using: composite\n  steps: []\n";
        auto p = cache.cachedPath("owner", "repo", "main");
        ASSERT(!p.empty());
        fs::remove_all(cache_dir);
    });

    // ── parseActionRef ────────────────────────────────────────────────────

    run("parseActionRef: owner/repo@ref", []() {
        std::string owner, repo, ref, subdir;
        bool ok = ActionRunner::parseActionRef("actions/checkout@v4", owner, repo, ref, subdir);
        ASSERT(ok);
        ASSERT_EQ(owner, "actions");
        ASSERT_EQ(repo,  "checkout");
        ASSERT_EQ(ref,   "v4");
        ASSERT(subdir.empty());
    });

    run("parseActionRef: owner/repo/subdir@ref", []() {
        std::string owner, repo, ref, subdir;
        bool ok = ActionRunner::parseActionRef("myorg/monorepo/my-action@main",
                                               owner, repo, ref, subdir);
        ASSERT(ok);
        ASSERT_EQ(owner,  "myorg");
        ASSERT_EQ(repo,   "monorepo");
        ASSERT_EQ(ref,    "main");
        ASSERT_EQ(subdir, "my-action");
    });

    run("parseActionRef: SHA ref", []() {
        std::string owner, repo, ref, subdir;
        bool ok = ActionRunner::parseActionRef("actions/setup-node@1234abcd", owner, repo, ref, subdir);
        ASSERT(ok);
        ASSERT_EQ(ref, "1234abcd");
    });

    run("parseActionRef: no @ returns false", []() {
        std::string owner, repo, ref, subdir;
        bool ok = ActionRunner::parseActionRef("actions/checkout", owner, repo, ref, subdir);
        ASSERT(!ok);
    });

    run("parseActionRef: no slash returns false", []() {
        std::string owner, repo, ref, subdir;
        bool ok = ActionRunner::parseActionRef("checkout@v4", owner, repo, ref, subdir);
        ASSERT(!ok);
    });

    // ── parseActionYaml: composite ────────────────────────────────────────

    run("parseActionYaml: composite action type", []() {
        auto dir = makeTempActionDir(R"(
name: My Composite Action
description: A test composite action
runs:
  using: composite
  steps:
    - id: step1
      run: echo hello
    - id: step2
      run: echo world
)");
        auto def = ActionRunner::parseActionYaml(dir / "action.yml");
        ASSERT_EQ(def.name, "My Composite Action");
        ASSERT_EQ(def.description, "A test composite action");
        ASSERT(def.type == ActionType::Composite);
        ASSERT_EQ(def.steps.size(), 2u);
        ASSERT_EQ(def.steps[0].id, "step1");
        ASSERT_EQ(def.steps[0].run, "echo hello");
        ASSERT_EQ(def.steps[1].id, "step2");
        removeTempActionDir();
    });

    run("parseActionYaml: composite steps count", []() {
        auto dir = makeTempActionDir(R"(
name: Setup
runs:
  using: composite
  steps:
    - run: apt-get install -y build-essential
    - run: cmake .
    - run: make
)");
        auto def = ActionRunner::parseActionYaml(dir / "action.yml");
        ASSERT_EQ(def.steps.size(), 3u);
        removeTempActionDir();
    });

    // ── parseActionYaml: javascript ────────────────────────────────────────

    run("parseActionYaml: node20 javascript action type", []() {
        auto dir = makeTempActionDir(R"(
name: My JS Action
description: Node 20 action
runs:
  using: node20
  main: dist/index.js
  pre: dist/pre.js
  post: dist/post.js
)");
        auto def = ActionRunner::parseActionYaml(dir / "action.yml");
        ASSERT(def.type == ActionType::JavaScript);
        ASSERT_EQ(def.using_runtime, "node20");
        ASSERT_EQ(def.main_script,   "dist/index.js");
        ASSERT_EQ(def.pre_script,    "dist/pre.js");
        ASSERT_EQ(def.post_script,   "dist/post.js");
        removeTempActionDir();
    });

    run("parseActionYaml: node16 also recognised as JavaScript", []() {
        auto dir = makeTempActionDir(R"(
name: Legacy JS
runs:
  using: node16
  main: index.js
)");
        auto def = ActionRunner::parseActionYaml(dir / "action.yml");
        ASSERT(def.type == ActionType::JavaScript);
        ASSERT_EQ(def.using_runtime, "node16");
        removeTempActionDir();
    });

    // ── parseActionYaml: docker ───────────────────────────────────────────

    run("parseActionYaml: docker action type", []() {
        auto dir = makeTempActionDir(R"(
name: Docker Action
runs:
  using: docker
  image: docker://ubuntu:22.04
)");
        auto def = ActionRunner::parseActionYaml(dir / "action.yml");
        ASSERT(def.type == ActionType::Docker);
        ASSERT_EQ(def.docker_image, "docker://ubuntu:22.04");
        removeTempActionDir();
    });

    // ── parseActionYaml: inputs and outputs ──────────────────────────────

    run("parseActionYaml: inputs parsed correctly", []() {
        auto dir = makeTempActionDir(R"(
name: Action With Inputs
inputs:
  token:
    description: GitHub token
    required: true
  path:
    description: Path to checkout
    default: '.'
    required: false
runs:
  using: composite
  steps: []
)");
        auto def = ActionRunner::parseActionYaml(dir / "action.yml");
        ASSERT_EQ(def.inputs.size(), 2u);
        ASSERT(def.inputs.count("token") > 0);
        ASSERT(def.inputs["token"].required == true);
        ASSERT_EQ(def.inputs["token"].description, "GitHub token");
        ASSERT(def.inputs.count("path") > 0);
        ASSERT_EQ(def.inputs["path"].default_value, ".");
        ASSERT(def.inputs["path"].required == false);
        removeTempActionDir();
    });

    run("parseActionYaml: outputs parsed correctly", []() {
        auto dir = makeTempActionDir(R"(
name: Action With Outputs
outputs:
  result:
    description: The result
    value: ${{ steps.compute.outputs.result }}
runs:
  using: composite
  steps: []
)");
        auto def = ActionRunner::parseActionYaml(dir / "action.yml");
        ASSERT_EQ(def.outputs.size(), 1u);
        ASSERT(def.outputs.count("result") > 0);
        ASSERT_EQ(def.outputs["result"].description, "The result");
        ASSERT_EQ(def.outputs["result"].value, "${{ steps.compute.outputs.result }}");
        removeTempActionDir();
    });

    run("parseActionYaml: missing file throws", []() {
        ASSERT_THROWS(ActionRunner::parseActionYaml("/nonexistent/path/action.yml"));
    });

    run("parseActionYaml: invalid YAML throws", []() {
        // Use a tab indentation error + scalar/mapping conflict — definitely invalid
        auto dir = makeTempActionDir("name: test\nruns:\n\t- bad_indent: [unclosed\n");
        ASSERT_THROWS(ActionRunner::parseActionYaml(dir / "action.yml"));
        removeTempActionDir();
    });

    run("parseActionYaml: step env parsed for composite", []() {
        auto dir = makeTempActionDir(R"(
name: Env Steps
runs:
  using: composite
  steps:
    - id: s1
      run: echo $MY_VAR
      env:
        MY_VAR: hello
)");
        auto def = ActionRunner::parseActionYaml(dir / "action.yml");
        ASSERT_EQ(def.steps.size(), 1u);
        ASSERT(def.steps[0].env.count("MY_VAR") > 0);
        ASSERT_EQ(def.steps[0].env.at("MY_VAR"), "hello");
        removeTempActionDir();
    });

    run("parseActionYaml: step with: inputs for composite nested uses", []() {
        auto dir = makeTempActionDir(R"(
name: Nested
runs:
  using: composite
  steps:
    - id: s1
      uses: actions/checkout@v4
      with:
        ref: main
        depth: '1'
)");
        auto def = ActionRunner::parseActionYaml(dir / "action.yml");
        ASSERT_EQ(def.steps.size(), 1u);
        ASSERT_EQ(def.steps[0].uses, "actions/checkout@v4");
        ASSERT_EQ(def.steps[0].with.at("ref"), "main");
        ASSERT_EQ(def.steps[0].with.at("depth"), "1");
        removeTempActionDir();
    });

    // ── Composite action $GITHUB_OUTPUT integration test ─────────────────
    // This test runs a real composite action locally (no network) and verifies
    // that a sub-step that writes to $GITHUB_OUTPUT propagates its output
    // back to the caller via StepRunResult.outputs.

    run("composite action: sub-step $GITHUB_OUTPUT flows back to caller", []() {
        // Create a temporary workspace + a local action directory
        fs::path ws = fs::temp_directory_path() / "act_runner_test_composite_ws";
        fs::path action_dir = ws / "my-action";
        fs::create_directories(ws);
        fs::create_directories(action_dir);

        // Write action.yml — composite with a step that sets an output
        std::ofstream yml(action_dir / "action.yml");
        yml << R"(
name: My Local Action
description: Test composite action output
outputs:
  greeting:
    description: A greeting
    value: ${{ steps.greet.outputs.greeting }}
runs:
  using: composite
  steps:
    - id: greet
      shell: /bin/sh
      run: |
        echo "greeting=hello-from-composite" >> "$GITHUB_OUTPUT"
)";
        yml.close();

        // Set up EnvManager, ActionCache, LogForwarder
        runner::EnvManager env_mgr(ws / "_runner");
        env_mgr.setup("", "push");

        fs::path cache_dir = ws / "_action_cache";
        runner::ActionCache cache(cache_dir);

        // Build a minimal base_env including GITHUB_OUTPUT
        auto base_env = env_mgr.currentEnv();

        // Minimal ExprContext
        runner::ExprContext ctx;

        // Sink log to string
        std::string log_buf;
        MockRunnerClient mock_client;
        runner::LogForwarder log_fwd(mock_client, 0, 50, 100,
            [&log_buf](const runner::LogLine& ll){ log_buf += ll.content + "\n"; });

        // Build a Step that uses: the local action
        runner::Step step;
        step.id   = "test-step";
        step.uses = "./my-action";

        // Run via ActionRunner
        runner::ActionRunner action_runner(ws, cache, "/bin/sh", "");
        auto result = action_runner.run(step, ctx, env_mgr, log_fwd, base_env);

        ASSERT(result.success);
        ASSERT(result.outputs.count("greeting") > 0);
        ASSERT_EQ(result.outputs.at("greeting"), "hello-from-composite");

        log_fwd.finish();
        env_mgr.cleanup();
        fs::remove_all(ws);
    });

    run("composite action: multiple sub-steps, later step output wins", []() {
        fs::path ws = fs::temp_directory_path() / "act_runner_test_composite_ws2";
        fs::path action_dir = ws / "multi-step-action";
        fs::create_directories(ws);
        fs::create_directories(action_dir);

        std::ofstream yml(action_dir / "action.yml");
        yml << R"(
name: Multi Step
runs:
  using: composite
  steps:
    - id: step1
      shell: /bin/sh
      run: |
        echo "value=first" >> "$GITHUB_OUTPUT"
    - id: step2
      shell: /bin/sh
      run: |
        echo "other=second" >> "$GITHUB_OUTPUT"
)";
        yml.close();

        runner::EnvManager env_mgr(ws / "_runner");
        env_mgr.setup();
        auto base_env = env_mgr.currentEnv();
        runner::ExprContext ctx;
        runner::ActionCache cache(ws / "_cache");
        MockRunnerClient mock_client2;
        runner::LogForwarder log_fwd(mock_client2, 0, 50, 100, nullptr);

        runner::Step step;
        step.id   = "s";
        step.uses = "./multi-step-action";

        runner::ActionRunner runner_obj(ws, cache, "/bin/sh", "");
        auto result = runner_obj.run(step, ctx, env_mgr, log_fwd, base_env);

        ASSERT(result.success);
        // Both outputs should be collected (accumulated across sub-steps)
        ASSERT(result.outputs.count("value") > 0);
        ASSERT(result.outputs.count("other") > 0);
        ASSERT_EQ(result.outputs.at("value"),  "first");
        ASSERT_EQ(result.outputs.at("other"),  "second");

        log_fwd.finish();
        env_mgr.cleanup();
        fs::remove_all(ws);
    });

    run("composite action: failing sub-step stops execution", []() {
        fs::path ws = fs::temp_directory_path() / "act_runner_test_composite_ws3";
        fs::path action_dir = ws / "fail-action";
        fs::create_directories(ws);
        fs::create_directories(action_dir);

        std::ofstream yml(action_dir / "action.yml");
        yml << R"(
name: Failing Action
runs:
  using: composite
  steps:
    - id: step1
      shell: /bin/sh
      run: exit 42
    - id: step2
      shell: /bin/sh
      run: echo "should not run"
)";
        yml.close();

        runner::EnvManager env_mgr(ws / "_runner");
        env_mgr.setup();
        auto base_env = env_mgr.currentEnv();
        runner::ExprContext ctx;
        runner::ActionCache cache(ws / "_cache");
        MockRunnerClient mock_client3;
        runner::LogForwarder log_fwd(mock_client3, 0, 50, 100, nullptr);

        runner::Step step;
        step.id   = "s";
        step.uses = "./fail-action";

        runner::ActionRunner runner_obj(ws, cache, "/bin/sh", "");
        auto result = runner_obj.run(step, ctx, env_mgr, log_fwd, base_env);

        ASSERT(!result.success);
        ASSERT_EQ(result.exit_code, 42);

        log_fwd.finish();
        env_mgr.cleanup();
        fs::remove_all(ws);
    });

    // ── Nested uses: in composite action ──────────────────────────────────

    run("nested uses: in composite action executes sub-action", []() {
        // Directory layout:
        //   ws/outer-action/action.yml   — composite, calls ./inner-action
        //   ws/inner-action/action.yml   — composite, runs echo + sets GITHUB_OUTPUT
        fs::path ws = fs::temp_directory_path() / "act_runner_nested_test";
        fs::remove_all(ws);
        fs::create_directories(ws / "outer-action");
        fs::create_directories(ws / "inner-action");

        // Inner action writes an output
        {
            std::ofstream f(ws / "inner-action" / "action.yml");
            f << R"(
name: Inner Action
runs:
  using: composite
  steps:
    - id: write
      shell: /bin/sh
      run: |
        echo "inner_out=hello_from_inner" >> $GITHUB_OUTPUT
)";
        }

        // Outer action calls inner action via nested uses:
        {
            std::ofstream f(ws / "outer-action" / "action.yml");
            f << R"(
name: Outer Action
outputs:
  forwarded:
    value: ${{ steps.inner.outputs.inner_out }}
runs:
  using: composite
  steps:
    - id: inner
      uses: ./inner-action
)";
        }

        runner::EnvManager env_mgr(ws / "_runner");
        env_mgr.setup();
        auto base_env = env_mgr.currentEnv();
        runner::ExprContext ctx;
        runner::ActionCache cache(ws / "_cache");
        MockRunnerClient mock_cl;
        runner::LogForwarder log_fwd(mock_cl, 0, 50, 100, nullptr);

        runner::Step step;
        step.id   = "outer";
        step.uses = "./outer-action";

        runner::ActionRunner ar(ws, cache, "/bin/sh", "");
        auto result = ar.run(step, ctx, env_mgr, log_fwd, base_env);
        log_fwd.finish();
        env_mgr.cleanup();
        fs::remove_all(ws);

        ASSERT(result.success);
        ASSERT(result.outputs.count("inner_out"));
        ASSERT_EQ(result.outputs.at("inner_out"), "hello_from_inner");
    });

    run("nested uses: failure propagates to outer composite", []() {
        fs::path ws = fs::temp_directory_path() / "act_runner_nested_fail";
        fs::remove_all(ws);
        fs::create_directories(ws / "outer-action");
        fs::create_directories(ws / "inner-action");

        // Inner action fails
        {
            std::ofstream f(ws / "inner-action" / "action.yml");
            f << R"(
name: Failing Inner
runs:
  using: composite
  steps:
    - id: fail
      shell: /bin/sh
      run: exit 5
)";
        }

        {
            std::ofstream f(ws / "outer-action" / "action.yml");
            f << R"(
name: Outer Calls Failing Inner
runs:
  using: composite
  steps:
    - id: inner
      uses: ./inner-action
    - id: should_not_run
      shell: /bin/sh
      run: echo "SHOULD_NOT_APPEAR"
)";
        }

        runner::EnvManager env_mgr(ws / "_runner");
        env_mgr.setup();
        auto base_env = env_mgr.currentEnv();
        runner::ExprContext ctx;
        runner::ActionCache cache(ws / "_cache");
        MockRunnerClient mock_cl2;
        runner::LogForwarder log_fwd(mock_cl2, 0, 50, 100, nullptr);

        runner::Step step;
        step.id   = "outer";
        step.uses = "./outer-action";

        runner::ActionRunner ar(ws, cache, "/bin/sh", "");
        auto result = ar.run(step, ctx, env_mgr, log_fwd, base_env);
        log_fwd.finish();
        env_mgr.cleanup();
        fs::remove_all(ws);

        ASSERT(!result.success);
        // "SHOULD_NOT_APPEAR" must not be in logs (step after failure skipped)
        std::string all_logs = mock_cl2.allLogs();
        ASSERT(all_logs.find("SHOULD_NOT_APPEAR") == std::string::npos);
    });

    run("nested uses: depth > 10 is rejected", []() {
        // Build 12 levels of nested composite actions (action11 → action10 → ... → action0)
        // The top-level call is at depth 0; action0 would be reached at depth 11,
        // which exceeds kMaxRecursionDepth (10) → must fail.
        fs::path ws = fs::temp_directory_path() / "act_runner_depth_test";
        fs::remove_all(ws);

        for (int i = 0; i <= 11; ++i) {
            fs::create_directories(ws / ("action" + std::to_string(i)));
            std::ofstream f(ws / ("action" + std::to_string(i)) / "action.yml");
            if (i == 0) {
                // Leaf: just runs echo
                f << "name: Leaf\nruns:\n  using: composite\n  steps:\n"
                  << "    - id: s\n      shell: /bin/sh\n      run: echo leaf\n";
            } else {
                // Calls action(i-1)
                f << "name: Level" << i << "\nruns:\n  using: composite\n  steps:\n"
                  << "    - id: s\n      uses: ./action" << (i-1) << "\n";
            }
        }

        runner::EnvManager env_mgr(ws / "_runner");
        env_mgr.setup();
        auto base_env = env_mgr.currentEnv();
        runner::ExprContext ctx;
        runner::ActionCache cache(ws / "_cache");
        MockRunnerClient mock_depth;
        runner::LogForwarder log_fwd(mock_depth, 0, 50, 100, nullptr);

        runner::Step step;
        step.id   = "top";
        step.uses = "./action11";   // 12 levels deep → exceeds kMaxRecursionDepth(10)

        runner::ActionRunner ar(ws, cache, "/bin/sh", "");
        auto result = ar.run(step, ctx, env_mgr, log_fwd, base_env);
        log_fwd.finish();
        env_mgr.cleanup();
        fs::remove_all(ws);

        ASSERT(!result.success);
        ASSERT(mock_depth.allLogs().find("recursion depth exceeded") != std::string::npos
            || mock_depth.allLogs().find("depth") != std::string::npos);
    });

    // ── post: script deferred execution ──────────────────────────────────
    // We cannot run a real JS action here (no node required in test env),
    // but we can verify the PostScript struct is populated correctly by
    // ActionRunner::run() when the action has a post: field.

    run("post: script is enqueued in post_queue when present", []() {
        // Build a mock JS action with main: and post: pointing to shell scripts
        // (we use /bin/sh scripts named .js — fine for testing the queue mechanism)
        fs::path ws = fs::temp_directory_path() / "act_runner_post_test";
        fs::remove_all(ws);
        fs::create_directories(ws / "my-action");

        // Write a minimal action.yml declaring a JS action with post:
        {
            std::ofstream f(ws / "my-action" / "action.yml");
            f << R"(
name: My Action
runs:
  using: node20
  main: main.js
  post: post.js
)";
        }
        // Write main.js and post.js (empty scripts — just need to exist)
        { std::ofstream f(ws / "my-action" / "main.js"); f << "// main\n"; }
        { std::ofstream f(ws / "my-action" / "post.js"); f << "// post\n"; }

        runner::EnvManager env_mgr(ws / "_runner");
        env_mgr.setup();
        auto base_env = env_mgr.currentEnv();
        runner::ExprContext ctx;
        runner::ActionCache cache(ws / "_cache");
        MockRunnerClient mock_cl;
        runner::LogForwarder log_fwd(mock_cl, 0, 50, 100, nullptr);

        runner::Step step;
        step.id   = "my-step";
        step.uses = "./my-action";

        std::vector<runner::PostScript> post_queue;
        runner::ActionRunner ar(ws, cache, "/bin/sh", "");

        // We expect this to either succeed (if node is on PATH) or fail
        // gracefully (if node is absent).  Either way, post_queue is populated.
        ar.run(step, ctx, env_mgr, log_fwd, base_env, &post_queue);
        log_fwd.finish();
        env_mgr.cleanup();
        fs::remove_all(ws);

        // The post: script should have been added to the queue
        ASSERT_EQ(post_queue.size(), 1u);
        ASSERT(post_queue[0].script_path.find("post.js") != std::string::npos);
    });

    run("post: queue is empty when action has no post: field", []() {
        fs::path ws = fs::temp_directory_path() / "act_runner_post_test2";
        fs::remove_all(ws);
        fs::create_directories(ws / "no-post-action");

        {
            std::ofstream f(ws / "no-post-action" / "action.yml");
            f << R"(
name: No Post
runs:
  using: node20
  main: main.js
)";
        }
        { std::ofstream f(ws / "no-post-action" / "main.js"); f << "// main\n"; }

        runner::EnvManager env_mgr(ws / "_runner");
        env_mgr.setup();
        auto base_env = env_mgr.currentEnv();
        runner::ExprContext ctx;
        runner::ActionCache cache(ws / "_cache");
        MockRunnerClient mock_cl;
        runner::LogForwarder log_fwd(mock_cl, 0, 50, 100, nullptr);

        runner::Step step;
        step.id   = "s";
        step.uses = "./no-post-action";

        std::vector<runner::PostScript> post_queue;
        runner::ActionRunner ar(ws, cache, "/bin/sh", "");
        ar.run(step, ctx, env_mgr, log_fwd, base_env, &post_queue);
        log_fwd.finish();
        env_mgr.cleanup();
        fs::remove_all(ws);

        ASSERT_EQ(post_queue.size(), 0u);
    });

    run("post: queue is populated in reverse when multiple JS steps", []() {
        // Two JS actions both with post: scripts → queue must have 2 entries.
        // TaskExecutor drains in reverse so the second step's post: runs first.
        fs::path ws = fs::temp_directory_path() / "act_runner_post_multi";
        fs::remove_all(ws);
        for (auto& name : {"action-a", "action-b"}) {
            fs::create_directories(ws / name);
            std::ofstream f(ws / name / "action.yml");
            f << "name: " << name << "\nruns:\n  using: node20\n"
              << "  main: main.js\n  post: post.js\n";
            std::ofstream m(ws / name / "main.js"); m << "//main\n";
            std::ofstream p(ws / name / "post.js"); p << "//post\n";
        }

        runner::EnvManager env_mgr(ws / "_runner");
        env_mgr.setup();
        auto base_env = env_mgr.currentEnv();
        runner::ExprContext ctx;
        runner::ActionCache cache(ws / "_cache");
        MockRunnerClient mock_cl;
        runner::LogForwarder log_fwd(mock_cl, 0, 50, 100, nullptr);

        std::vector<runner::PostScript> post_queue;
        runner::ActionRunner ar(ws, cache, "/bin/sh", "");

        for (auto& name : {"action-a", "action-b"}) {
            runner::Step step;
            step.id   = name;
            step.uses = std::string("./") + name;
            ar.run(step, ctx, env_mgr, log_fwd, base_env, &post_queue);
        }

        log_fwd.finish();
        env_mgr.cleanup();
        fs::remove_all(ws);

        ASSERT_EQ(post_queue.size(), 2u);
        // First entry is action-a's post:, second is action-b's post:
        // TaskExecutor reverses before draining (action-b runs first)
        ASSERT(post_queue[0].script_path.find("action-a") != std::string::npos);
        ASSERT(post_queue[1].script_path.find("action-b") != std::string::npos);
    });

    // ── actions/checkout built-in stub ────────────────────────────────────

    // Test 30: actions/checkout@v4 with no git binary emits a warning and
    // returns success (non-fatal for minimal CI environments).
    // We simulate "no git" by placing a fake git stub on PATH that exits 127.
    run("actions/checkout: no-git emits warning and succeeds", []() {
        namespace fs = std::filesystem;
        fs::path ws = fs::temp_directory_path() / "act_runner_checkout_no_git";
        fs::create_directories(ws / "_runner");
        fs::create_directories(ws / "workspace");

        // Simulate "no git" by placing a fake git stub at the front of PATH
        // that exits non-zero, shadowing the real /bin/git.
        fs::path fake_bin = ws / "fake_bin";
        fs::create_directories(fake_bin);
        {
            std::ofstream stub(fake_bin / "git");
            stub << "#!/bin/sh\nexit 127\n";
        }
        ::chmod((fake_bin / "git").string().c_str(), 0755);
        std::string orig_path = getenv("PATH") ? getenv("PATH") : "/bin:/usr/bin";
        // fake_bin first so our stub shadows /bin/git
        ::setenv("PATH", (fake_bin.string() + ":" + orig_path).c_str(), 1);

        runner::EnvManager env_mgr(ws / "_runner");
        env_mgr.setup();
        auto base_env = env_mgr.currentEnv();

        runner::ExprContext ctx;
        ctx.setString("github.repository", "owner/repo");
        ctx.setString("github.ref", "refs/heads/main");
        ctx.setString("github.server_url", "https://github.com");
        ctx.setString("github.token", "");

        runner::ActionCache cache(ws / "_cache");
        MockRunnerClient mock_cl;
        std::vector<std::string> log_lines;
        runner::LogForwarder log_fwd(mock_cl, 0, 50, 100,
            [&](const runner::LogLine& ll) { log_lines.push_back(ll.content); });

        runner::ActionRunner ar(ws / "workspace", cache, "/bin/sh",
                                "https://github.com");

        runner::Step step;
        step.id   = "checkout";
        step.uses = "actions/checkout@v4";

        auto result = ar.run(step, ctx, env_mgr, log_fwd, base_env, nullptr);

        // Restore PATH before doing anything that might need it
        ::setenv("PATH", orig_path.c_str(), 1);

        log_fwd.finish();
        env_mgr.cleanup();
        fs::remove_all(ws);

        // With no git in PATH the handler should succeed non-fatally with a warning.
        ASSERT(result.success);

        bool has_warning = false;
        for (auto& l : log_lines) {
            if (l.find("warning") != std::string::npos ||
                l.find("git")     != std::string::npos) {
                has_warning = true;
                break;
            }
        }
        ASSERT(has_warning);
    });

    // Test 31: actions/checkout base-name extraction ignores @version.
    // Verify @v1..@v4 are all recognised as built-in checkout (no
    // "Invalid action reference" exception).  We use the same fake-PATH
    // trick so git is not found, meaning the handler returns success immediately
    // without attempting any network I/O.
    run("actions/checkout: @v1 @v2 @v3 @v4 all recognised as checkout", []() {
        namespace fs = std::filesystem;
        fs::path ws = fs::temp_directory_path() / "act_runner_checkout_ver";
        fs::create_directories(ws / "_runner");
        fs::create_directories(ws / "workspace");

        // Place a fake git stub that exits 127 at the front of PATH so it
        // shadows /bin/git.  The checkout handler detects git failure and
        // returns success immediately without any network I/O.
        fs::path fake_bin = ws / "fake_bin";
        fs::create_directories(fake_bin);
        {
            std::ofstream stub(fake_bin / "git");
            stub << "#!/bin/sh\nexit 127\n";
        }
        ::chmod((fake_bin / "git").string().c_str(), 0755);
        std::string orig_path = getenv("PATH") ? getenv("PATH") : "/bin:/usr/bin";
        ::setenv("PATH", (fake_bin.string() + ":" + orig_path).c_str(), 1);

        runner::EnvManager env_mgr(ws / "_runner");
        env_mgr.setup();
        runner::ExprContext ctx;
        ctx.setString("github.repository", "test/repo");
        ctx.setString("github.ref", "refs/heads/main");
        ctx.setString("github.server_url", "https://nonexistent.invalid");
        ctx.setString("github.token", "");

        runner::ActionCache cache(ws / "_cache");
        MockRunnerClient mock_cl;
        runner::LogForwarder log_fwd(mock_cl, 0, 50, 100, nullptr);

        runner::ActionRunner ar(ws / "workspace", cache, "/bin/sh",
                                "https://nonexistent.invalid");

        for (const char* ver : {"@v1", "@v2", "@v3", "@v4"}) {
            runner::Step step;
            step.id   = "checkout";
            step.uses = std::string("actions/checkout") + ver;
            bool threw_invalid_ref = false;
            ActionRunResult result;
            try {
                result = ar.run(step, ctx, env_mgr, log_fwd, {}, nullptr);
            } catch (const std::exception& e) {
                std::string msg = e.what();
                if (msg.find("Invalid action reference") != std::string::npos)
                    threw_invalid_ref = true;
            }
            // Built-in handler invoked → no "invalid action reference"
            ASSERT(!threw_invalid_ref);
            // With no working git the handler returns success non-fatally
            ASSERT(result.success);
        }

        ::setenv("PATH", orig_path.c_str(), 1);
        log_fwd.finish();
        env_mgr.cleanup();
        fs::remove_all(ws);
    });

    return summary();
}
