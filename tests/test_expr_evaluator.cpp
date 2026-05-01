// tests/test_expr_evaluator.cpp — Unit tests for the expression evaluator
#include "test_runner.h"
#include "../src/workflow/ExprEvaluator.h"

#include <filesystem>
#include <fstream>
#include <cctype>

using namespace runner;
using namespace test;
namespace fs = std::filesystem;

static ExprContext makeCtx() {
    ExprContext ctx;
    ctx.setString("github.ref",           "refs/heads/main");
    ctx.setString("github.event_name",    "push");
    ctx.setString("github.sha",           "abc123def456");
    ctx.setString("github.repository",    "octocat/hello-world");
    ctx.setString("env.MY_VAR",           "hello");
    ctx.setString("runner.os",            "haiku");
    ctx.setString("steps.checkout.outcome", "success");
    ctx.setString("steps.build.outputs.artifact", "app.zip");
    ctx.set("some.number",                ExprValue::number(42.0));
    ctx.set("some.flag",                  ExprValue::boolean(true));
    return ctx;
}

int main() {
    std::cout << "=== ExprEvaluator tests ===\n\n";

    run("literal string", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        ASSERT_EQ(eval.evaluateExpr("'hello world'").toString(), "hello world");
    });

    run("literal number", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        ASSERT_EQ(eval.evaluateExpr("42").toNumber(), 42.0);
    });

    run("literal bool true", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("true").isTruthy());
    });

    run("literal bool false", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        ASSERT(!eval.evaluateExpr("false").isTruthy());
    });

    run("literal null", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        ASSERT(!eval.evaluateExpr("null").isTruthy());
        ASSERT(eval.evaluateExpr("null").type == ExprValue::Type::Null);
    });

    run("context lookup - string", []() {
        auto ctx = makeCtx();
        ExprEvaluator eval(ctx);
        ASSERT_EQ(eval.evaluateExpr("github.ref").toString(), "refs/heads/main");
    });

    run("context lookup - number", []() {
        auto ctx = makeCtx();
        ExprEvaluator eval(ctx);
        ASSERT_EQ(eval.evaluateExpr("some.number").toNumber(), 42.0);
    });

    run("context lookup - bool", []() {
        auto ctx = makeCtx();
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("some.flag").isTruthy());
    });

    run("context lookup - missing key returns null", []() {
        auto ctx = makeCtx();
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("github.missing").type == ExprValue::Type::Null);
    });

    run("equality == (matching)", []() {
        auto ctx = makeCtx();
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("github.event_name == 'push'").isTruthy());
    });

    run("equality == (non-matching)", []() {
        auto ctx = makeCtx();
        ExprEvaluator eval(ctx);
        ASSERT(!eval.evaluateExpr("github.event_name == 'pull_request'").isTruthy());
    });

    run("inequality !=", []() {
        auto ctx = makeCtx();
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("github.event_name != 'release'").isTruthy());
    });

    run("logical &&", []() {
        auto ctx = makeCtx();
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("true && true").isTruthy());
        ASSERT(!eval.evaluateExpr("true && false").isTruthy());
    });

    run("logical ||", []() {
        auto ctx = makeCtx();
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("false || true").isTruthy());
        ASSERT(!eval.evaluateExpr("false || false").isTruthy());
    });

    run("logical !", []() {
        auto ctx = makeCtx();
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("!false").isTruthy());
        ASSERT(!eval.evaluateExpr("!true").isTruthy());
    });

    run("parenthesised expression", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("(true || false) && true").isTruthy());
    });

    run("contains(string, substr)", []() {
        auto ctx = makeCtx();
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("contains(github.ref, 'main')").isTruthy());
        ASSERT(!eval.evaluateExpr("contains(github.ref, 'feature')").isTruthy());
    });

    run("contains() case-insensitive", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("contains('Hello World', 'hello')").isTruthy());
    });

    run("startsWith()", []() {
        auto ctx = makeCtx();
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("startsWith(github.ref, 'refs/heads/')").isTruthy());
        ASSERT(!eval.evaluateExpr("startsWith(github.ref, 'refs/tags/')").isTruthy());
    });

    run("endsWith()", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("endsWith('hello.zip', '.zip')").isTruthy());
        ASSERT(!eval.evaluateExpr("endsWith('hello.tar', '.zip')").isTruthy());
    });

    run("format() with {0} placeholders", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        auto result = eval.evaluateExpr("format('Hello {0}!', 'World')").toString();
        ASSERT_EQ(result, "Hello World!");
    });

    run("format() with multiple placeholders", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        auto result = eval.evaluateExpr("format('{0}/{1}', 'owner', 'repo')").toString();
        ASSERT_EQ(result, "owner/repo");
    });

    run("toJSON(string)", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        ASSERT_EQ(eval.evaluateExpr("toJSON('hello')").toString(), "\"hello\"");
    });

    run("always() returns true", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("always()").isTruthy());
    });

    run("success() default true", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("success()").isTruthy());
    });

    run("failure() after job failure", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        eval.setJobFailure(true);
        eval.setJobSuccess(false);
        ASSERT(eval.evaluateExpr("failure()").isTruthy());
        ASSERT(!eval.evaluateExpr("success()").isTruthy());
    });

    run("interpolate: no expressions", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        ASSERT_EQ(eval.interpolate("plain text"), "plain text");
    });

    run("interpolate: single expression", []() {
        auto ctx = makeCtx();
        ExprEvaluator eval(ctx);
        ASSERT_EQ(eval.interpolate("Branch: ${{ github.ref }}"),
                  "Branch: refs/heads/main");
    });

    run("interpolate: multiple expressions", []() {
        auto ctx = makeCtx();
        ExprEvaluator eval(ctx);
        auto result = eval.interpolate("${{ github.repository }} on ${{ runner.os }}");
        ASSERT_EQ(result, "octocat/hello-world on haiku");
    });

    run("evaluateCondition: plain true", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateCondition("true"));
    });

    run("evaluateCondition: empty string = true (default)", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateCondition(""));
    });

    run("evaluateCondition: ${{ }} wrapped", []() {
        auto ctx = makeCtx();
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateCondition("${{ github.event_name == 'push' }}"));
    });

    run("numeric comparison <", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("1 < 2").isTruthy());
        ASSERT(!eval.evaluateExpr("2 < 1").isTruthy());
    });

    run("numeric comparison >=", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("2 >= 2").isTruthy());
        ASSERT(eval.evaluateExpr("3 >= 2").isTruthy());
        ASSERT(!eval.evaluateExpr("1 >= 2").isTruthy());
    });

    run("string equality case-insensitive", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        ASSERT(eval.evaluateExpr("'Hello' == 'hello'").isTruthy());
    });

    run("steps output access", []() {
        auto ctx = makeCtx();
        ExprEvaluator eval(ctx);
        ASSERT_EQ(eval.evaluateExpr("steps.build.outputs.artifact").toString(),
                  "app.zip");
    });

    // ── hashFiles() ──────────────────────────────────────────────────────────

    run("hashFiles: no patterns returns empty string", []() {
        ExprContext ctx;
        ExprEvaluator eval(ctx);
        auto result = eval.evaluateExpr("hashFiles()");
        ASSERT_EQ(result.toString(), "");
    });

    run("hashFiles: non-matching pattern returns empty string", []() {
        ExprContext ctx;
        ctx.setString("env.GITHUB_WORKSPACE", "/tmp");
        ExprEvaluator eval(ctx);
        auto result = eval.evaluateExpr("hashFiles('this_file_does_not_exist_xyz_abc.lock')");
        ASSERT_EQ(result.toString(), "");
    });

    run("hashFiles: matching file returns 64-char hex string", []() {
        // Create a temporary file
        std::string path = "/tmp/act_runner_hashfiles_test.txt";
        { std::ofstream f(path); f << "hello hashFiles\n"; }

        ExprContext ctx;
        ctx.setString("env.GITHUB_WORKSPACE", "/tmp");
        ExprEvaluator eval(ctx);
        // Use the exact filename (no glob needed)
        auto result = eval.evaluateExpr("hashFiles('act_runner_hashfiles_test.txt')");

        // Result should be 64 hex chars
        ASSERT_EQ(result.toString().size(), 64u);
        // Should be all hex characters
        for (char c : result.toString()) {
            ASSERT(std::isxdigit(c));
        }
        std::remove(path.c_str());
    });

    run("hashFiles: same content produces same hash", []() {
        std::string path1 = "/tmp/act_runner_hf_a.txt";
        std::string path2 = "/tmp/act_runner_hf_b.txt";
        { std::ofstream f(path1); f << "same content"; }
        { std::ofstream f(path2); f << "same content"; }

        ExprContext ctx;
        ctx.setString("env.GITHUB_WORKSPACE", "/tmp");
        ExprEvaluator eval(ctx);

        auto h1 = eval.evaluateExpr("hashFiles('act_runner_hf_a.txt')").toString();
        auto h2 = eval.evaluateExpr("hashFiles('act_runner_hf_b.txt')").toString();

        ASSERT_EQ(h1.size(), 64u);
        ASSERT_EQ(h1, h2);   // same content → same hash

        std::remove(path1.c_str());
        std::remove(path2.c_str());
    });

    run("hashFiles: different content produces different hash", []() {
        std::string path_a = "/tmp/act_runner_hf_diff_a.txt";
        std::string path_b = "/tmp/act_runner_hf_diff_b.txt";
        { std::ofstream f(path_a); f << "content A"; }
        { std::ofstream f(path_b); f << "content B"; }

        ExprContext ctx;
        ctx.setString("env.GITHUB_WORKSPACE", "/tmp");
        ExprEvaluator eval(ctx);

        auto ha = eval.evaluateExpr("hashFiles('act_runner_hf_diff_a.txt')").toString();
        auto hb = eval.evaluateExpr("hashFiles('act_runner_hf_diff_b.txt')").toString();

        ASSERT(ha != hb);

        std::remove(path_a.c_str());
        std::remove(path_b.c_str());
    });

    run("hashFiles: two patterns covering same files = same hash as one", []() {
        std::string path = "/tmp/act_runner_hf_multi.txt";
        { std::ofstream f(path); f << "multi pattern test"; }

        ExprContext ctx;
        ctx.setString("env.GITHUB_WORKSPACE", "/tmp");
        ExprEvaluator eval(ctx);

        // hashFiles with the same pattern twice should deduplicate
        auto h1 = eval.evaluateExpr("hashFiles('act_runner_hf_multi.txt')").toString();
        auto h2 = eval.evaluateExpr("hashFiles('act_runner_hf_multi.txt', 'act_runner_hf_multi.txt')").toString();

        ASSERT_EQ(h1, h2);   // deduplication ensures identical result

        std::remove(path.c_str());
    });

    run("hashFiles: result is a string ExprValue", []() {
        std::string path = "/tmp/act_runner_hf_type.txt";
        { std::ofstream f(path); f << "type check"; }

        ExprContext ctx;
        ctx.setString("env.GITHUB_WORKSPACE", "/tmp");
        ExprEvaluator eval(ctx);
        auto v = eval.evaluateExpr("hashFiles('act_runner_hf_type.txt')");
        ASSERT(v.type == ExprValue::Type::String);

        std::remove(path.c_str());
    });

    run("hashFiles: interpolated in ${{ }} context", []() {
        std::string path = "/tmp/act_runner_hf_interp.txt";
        { std::ofstream f(path); f << "interpolation test"; }

        ExprContext ctx;
        ctx.setString("env.GITHUB_WORKSPACE", "/tmp");
        ExprEvaluator eval(ctx);
        std::string result = eval.interpolate(
            "hash=${{ hashFiles('act_runner_hf_interp.txt') }}");

        ASSERT(result.substr(0, 5) == "hash=");
        ASSERT_EQ(result.size(), 5u + 64u);   // "hash=" + 64 hex chars

        std::remove(path.c_str());
    });

    // ── hashFiles with ** recursive glob ─────────────────────────────────

    run("hashFiles: ** glob matches files in subdirectories", []() {
        // Build a small tree under /tmp/hf_tree/:
        //   /tmp/hf_tree/a.lock
        //   /tmp/hf_tree/sub/b.lock
        //   /tmp/hf_tree/sub/c.txt   (should NOT match *.lock pattern)
        fs::path tree = "/tmp/hf_tree";
        fs::create_directories(tree / "sub");
        { std::ofstream f(tree / "a.lock");     f << "content-a"; }
        { std::ofstream f(tree / "sub/b.lock"); f << "content-b"; }
        { std::ofstream f(tree / "sub/c.txt");  f << "content-c"; }

        ExprContext ctx;
        ctx.setString("env.GITHUB_WORKSPACE", tree.string());
        ExprEvaluator eval(ctx);

        // Match all .lock files recursively
        auto result = eval.evaluateExpr("hashFiles('**/*.lock')");
        std::string hash = result.toString();

        // Should produce a 64-char hex hash (both a.lock and b.lock matched)
        ASSERT_EQ(hash.size(), 64u);

        // Matching only the top-level .lock should produce a different hash
        auto result2 = eval.evaluateExpr("hashFiles('*.lock')");
        std::string hash2 = result2.toString();
        // hash2 covers only a.lock; should differ from hash (which covers a+b)
        ASSERT(hash != hash2);
        ASSERT_EQ(hash2.size(), 64u);

        fs::remove_all(tree);
    });

    run("hashFiles: ** with no matches returns empty string", []() {
        fs::path tree = "/tmp/hf_empty_tree";
        fs::create_directories(tree);
        { std::ofstream f(tree / "readme.md"); f << "hello"; }

        ExprContext ctx;
        ctx.setString("env.GITHUB_WORKSPACE", tree.string());
        ExprEvaluator eval(ctx);

        auto result = eval.evaluateExpr("hashFiles('**/*.lock')");
        // No .lock files → empty string
        ASSERT_EQ(result.toString(), std::string(""));

        fs::remove_all(tree);
    });

    return summary();
}
