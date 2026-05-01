// tests/test_context_builder.cpp — Unit tests for ContextBuilder
#include "test_runner.h"
#include "../src/workflow/ContextBuilder.h"
#include "../src/workflow/ExprEvaluator.h"
#include "../src/client/RunnerClient.h"

using namespace test;
using namespace runner;

// Helper: make a minimal TaskDto with the given context/secrets/vars
// (TaskDto uses vector<pair> for these fields)
static TaskDto makeTask(
    int64_t id = 1,
    std::vector<std::pair<std::string,std::string>> ctx  = {},
    std::vector<std::pair<std::string,std::string>> sec  = {},
    std::vector<std::pair<std::string,std::string>> vars = {})
{
    TaskDto t;
    t.id      = id;
    t.machine = "build";
    t.context = std::move(ctx);
    t.secrets = std::move(sec);
    t.vars    = std::move(vars);
    return t;
}

// Convenience: look up a dotted context path and return its ExprValue
static ExprValue ctxGet(const ExprContext& ctx, const std::string& path) {
    return ctx.get(path);
}

int main() {
    std::cout << "=== ContextBuilder tests ===\n\n";

    // ── runner.* ──────────────────────────────────────────────────────────

    run("runner.name is set from withRunnerInfo", []() {
        ContextBuilder b;
        b.withRunnerInfo("my-haiku-runner");
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "runner.name").toString(), "my-haiku-runner");
    });

    run("runner.os defaults to haiku", []() {
        ContextBuilder b;
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "runner.os").toString(), "haiku");
    });

    run("runner.arch defaults to X64", []() {
        ContextBuilder b;
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "runner.arch").toString(), "X64");
    });

    run("runner.name defaults when not set", []() {
        ContextBuilder b;
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "runner.name").toString(), "haiku-runner");
    });

    // ── github.* from flat context keys ─────────────────────────────────

    run("github.ref is set from flat context key", []() {
        auto task = makeTask(1, {{"ref", "refs/heads/main"}});
        ContextBuilder b;
        b.withTask(task);
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "github.ref").toString(), "refs/heads/main");
    });

    run("github.sha is set from flat context key", []() {
        auto task = makeTask(1, {{"sha", "abc123def456"}});
        ContextBuilder b;
        b.withTask(task);
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "github.sha").toString(), "abc123def456");
    });

    run("github.actor is set from context", []() {
        auto task = makeTask(1, {{"actor", "johndoe"}});
        ContextBuilder b;
        b.withTask(task);
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "github.actor").toString(), "johndoe");
    });

    run("multiple github context keys all set", []() {
        auto task = makeTask(1, {
            {"ref",        "refs/tags/v1.0"},
            {"sha",        "deadbeef"},
            {"event_name", "push"},
        });
        ContextBuilder b;
        b.withTask(task);
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "github.ref").toString(),        "refs/tags/v1.0");
        ASSERT_EQ(ctxGet(ctx, "github.sha").toString(),        "deadbeef");
        ASSERT_EQ(ctxGet(ctx, "github.event_name").toString(), "push");
    });

    // ── secrets.* and vars.* ─────────────────────────────────────────────

    run("secrets are available in context", []() {
        auto task = makeTask(1, {}, {{"MY_TOKEN", "s3cr3t"}});
        ContextBuilder b;
        b.withTask(task);
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "secrets.MY_TOKEN").toString(), "s3cr3t");
    });

    run("vars are available in context", []() {
        auto task = makeTask(1, {}, {}, {{"DEPLOY_ENV", "production"}});
        ContextBuilder b;
        b.withTask(task);
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "vars.DEPLOY_ENV").toString(), "production");
    });

    // ── env.* ─────────────────────────────────────────────────────────────

    run("env context is set from withJobEnv", []() {
        ContextBuilder b;
        b.withJobEnv({{"MY_VAR", "hello"}, {"CI", "true"}});
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "env.MY_VAR").toString(), "hello");
        ASSERT_EQ(ctxGet(ctx, "env.CI").toString(),     "true");
    });

    // ── steps.* ───────────────────────────────────────────────────────────

    run("steps.id.outcome is success after success", []() {
        ContextBuilder b;
        b.addStepOutputs("build", 1 /*success*/, {});
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "steps.build.outcome").toString(), "success");
    });

    run("steps.id.outcome is failure after failure", []() {
        ContextBuilder b;
        b.addStepOutputs("test", 2 /*failure*/, {});
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "steps.test.outcome").toString(), "failure");
    });

    run("steps.id.conclusion matches outcome", []() {
        ContextBuilder b;
        b.addStepOutputs("lint", 1 /*success*/, {});
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "steps.lint.conclusion").toString(), "success");
    });

    run("step outputs accessible via steps.id.outputs.key", []() {
        ContextBuilder b;
        b.addStepOutputs("build", 1, {{"version", "1.2.3"}, {"artifact", "bin.tar.gz"}});
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "steps.build.outputs.version").toString(),  "1.2.3");
        ASSERT_EQ(ctxGet(ctx, "steps.build.outputs.artifact").toString(), "bin.tar.gz");
    });

    run("multiple steps tracked independently", []() {
        ContextBuilder b;
        b.addStepOutputs("checkout", 1, {});
        b.addStepOutputs("build",    1, {{"binary", "app"}});
        b.addStepOutputs("test",     2, {});
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "steps.checkout.outcome").toString(),      "success");
        ASSERT_EQ(ctxGet(ctx, "steps.build.outputs.binary").toString(),  "app");
        ASSERT_EQ(ctxGet(ctx, "steps.test.outcome").toString(),          "failure");
    });

    // ── matrix.* ─────────────────────────────────────────────────────────

    run("matrix values available in context", []() {
        ContextBuilder b;
        b.withMatrix({{"os", "haiku"}, {"version", "r1beta5"}});
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "matrix.os").toString(),      "haiku");
        ASSERT_EQ(ctxGet(ctx, "matrix.version").toString(), "r1beta5");
    });

    run("matrix missing key returns null", []() {
        ContextBuilder b;
        b.withMatrix({{"os", "haiku"}});
        auto ctx = b.build();
        ASSERT(ctxGet(ctx, "matrix.nonexistent").isTruthy() == false);
        // null isTruthy() is false
    });

    // ── needs.* (from TaskDto.needs_context) ─────────────────────────────

    run("needs.<job>.result populated from task.needs_context", []() {
        TaskDto t = makeTask();
        StepContextDto sc;
        sc.id = "build";
        sc.result = 1; // success
        t.needs_context.push_back(sc);

        ContextBuilder b;
        b.withTask(t);
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "needs.build.result").toString(), "success");
    });

    run("needs.<job>.outputs.* accessible", []() {
        TaskDto t = makeTask();
        StepContextDto sc;
        sc.id = "build";
        sc.result = 1;
        sc.outputs = {{"version", "1.2.3"}, {"sha", "abc"}};
        t.needs_context.push_back(sc);

        ContextBuilder b;
        b.withTask(t);
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "needs.build.outputs.version").toString(), "1.2.3");
        ASSERT_EQ(ctxGet(ctx, "needs.build.outputs.sha").toString(),     "abc");
    });

    run("multiple upstream jobs tracked independently in needs.*", []() {
        TaskDto t = makeTask();
        StepContextDto a; a.id = "lint";  a.result = 1;
        StepContextDto b; b.id = "build"; b.result = 2;  // failure
        b.outputs = {{"artifact", "app.tar"}};
        t.needs_context = {a, b};

        ContextBuilder cb;
        cb.withTask(t);
        auto ctx = cb.build();
        ASSERT_EQ(ctxGet(ctx, "needs.lint.result").toString(),              "success");
        ASSERT_EQ(ctxGet(ctx, "needs.build.result").toString(),             "failure");
        ASSERT_EQ(ctxGet(ctx, "needs.build.outputs.artifact").toString(),   "app.tar");
    });

    run("expression 'needs.build.result == success' works", []() {
        TaskDto t = makeTask();
        StepContextDto sc; sc.id = "build"; sc.result = 1;
        t.needs_context.push_back(sc);
        ContextBuilder b;
        b.withTask(t);
        auto ctx = b.build();
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateCondition("needs.build.result == 'success'"));
    });

    // ── github.* extraction skips matrix/event/event_payload keys ────────

    run("matrix key in task.context does not leak to github.matrix", []() {
        // Server may send task.context["matrix"] = {"os":"haiku"} (JSON)
        // ContextBuilder should skip it (TaskExecutor handles it via
        // withMatrix() instead).
        auto task = makeTask(1, {
            {"sha",    "abc"},
            {"matrix", "{\"os\":\"haiku\"}"},
        });
        ContextBuilder b;
        b.withTask(task);
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "github.sha").toString(), "abc");
        // github.matrix should NOT be set (matrix key was skipped)
        ASSERT(ctxGet(ctx, "github.matrix").isTruthy() == false);
        ASSERT(ctxGet(ctx, "github.os").isTruthy()     == false);
    });

    run("event_payload skipped but event_name preserved as github.event_name", []() {
        auto task = makeTask(1, {
            {"event_name",    "push"},
            {"event_payload", "{\"ref\":\"main\"}"},
        });
        ContextBuilder b;
        b.withTask(task);
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "github.event_name").toString(), "push");
        // event_payload JSON was big and would have exploded into github.*
        ASSERT(ctxGet(ctx, "github.ref").isTruthy() == false);
    });

    // ── build() can be called multiple times ────────────────────────────

    run("build() is idempotent", []() {
        ContextBuilder b;
        b.withRunnerInfo("runner1");
        auto ctx1 = b.build();
        auto ctx2 = b.build();
        ASSERT_EQ(ctxGet(ctx1, "runner.name").toString(),
                  ctxGet(ctx2, "runner.name").toString());
    });

    run("addStepOutputs accumulates across calls", []() {
        ContextBuilder b;
        b.addStepOutputs("step1", 1, {{"key1", "v1"}});
        b.addStepOutputs("step2", 1, {{"key2", "v2"}});
        b.addStepOutputs("step3", 2, {});
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "steps.step1.outputs.key1").toString(), "v1");
        ASSERT_EQ(ctxGet(ctx, "steps.step2.outputs.key2").toString(), "v2");
        ASSERT_EQ(ctxGet(ctx, "steps.step3.outcome").toString(),      "failure");
    });

    // ── withTask + withRunnerInfo combined ───────────────────────────────

    run("withTask and withRunnerInfo coexist", []() {
        auto task = makeTask(42,
            {{"sha", "cafebabe"}},
            {{"TOKEN", "xyz"}});
        ContextBuilder b;
        b.withTask(task)
         .withRunnerInfo("haiku-ci", "haiku", "x86_64")
         .withJobEnv({{"BUILD_TYPE", "release"}});
        auto ctx = b.build();
        ASSERT_EQ(ctxGet(ctx, "github.sha").toString(),     "cafebabe");
        ASSERT_EQ(ctxGet(ctx, "secrets.TOKEN").toString(),  "xyz");
        ASSERT_EQ(ctxGet(ctx, "runner.name").toString(),    "haiku-ci");
        ASSERT_EQ(ctxGet(ctx, "env.BUILD_TYPE").toString(), "release");
    });

    // ── expression interpolation via ExprEvaluator + built context ───────

    run("ExprEvaluator can interpolate runner.os from built context", []() {
        ContextBuilder b;
        b.withRunnerInfo("test-runner", "haiku", "x86_64");
        auto ctx = b.build();
        ExprEvaluator eval(ctx);
        // ${{ runner.os }} should interpolate to "haiku"
        std::string result = eval.evaluate("OS is ${{ runner.os }}");
        ASSERT_EQ(result, "OS is haiku");
    });

    run("ExprEvaluator can interpolate step outputs", []() {
        ContextBuilder b;
        b.addStepOutputs("setup", 1, {{"version", "2.0.1"}});
        auto ctx = b.build();
        ExprEvaluator eval(ctx);
        std::string result = eval.evaluate("v${{ steps.setup.outputs.version }}");
        ASSERT_EQ(result, "v2.0.1");
    });

    run("ExprEvaluator evaluates if condition from context", []() {
        ContextBuilder b;
        b.addStepOutputs("build", 1 /*success*/, {});
        auto ctx = b.build();
        ExprEvaluator eval(ctx);
        // "success()" uses the evaluator's internal job status, not context
        // but a plain comparison should work:
        bool res = eval.evaluateCondition("steps.build.outcome == 'success'");
        ASSERT(res);
    });

    run("github.token populated from gitea_runtime_token", []() {
        TaskDto task;
        task.gitea_runtime_token = "my-secret-runtime-token";
        ContextBuilder b;
        b.withTask(task);
        auto ctx = b.build();
        ExprEvaluator eval(ctx);
        auto val = eval.evaluateExpr("github.token");
        ASSERT_EQ(val.toString(), "my-secret-runtime-token");
    });

    run("github.token empty when no runtime token", []() {
        TaskDto task;
        task.gitea_runtime_token = "";
        ContextBuilder b;
        b.withTask(task);
        auto ctx = b.build();
        ExprEvaluator eval(ctx);
        // No token → github.token should be null/empty
        auto val = eval.evaluateExpr("github.token");
        ASSERT(!val.isTruthy());
    });

    return summary();
}
