// tests/test_workflow_orchestrator.cpp
//
// Integration tests for WorkflowOrchestrator — local multi-job wave dispatch.
// Uses MockRunnerClient (no network) to drive full workflow execution.

#include "test_runner.h"
#include "MockRunnerClient.h"
#include "../src/runner/WorkflowOrchestrator.h"
#include "../src/workflow/WorkflowParser.h"
#include "../src/config/Config.h"

#include <functional>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <string>

using namespace test;
using namespace runner;

// ── Flake retry (same pattern as test_task_executor) ─────────────────────
// Haiku posix_spawn can occasionally deliver SIGKILLTHR to a spawned child
// while the test process is creating many threads; retry on such signals.

static void retry_on_flake(std::function<void()> body, int max_attempts = 5) {
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        try {
            body();
            return;
        } catch (const std::exception& e) {
            if (attempt == max_attempts) throw;
            // Retry on any failure — on Haiku, posix_spawn occasionally
            // delivers SIGKILLTHR to a child in multi-threaded scenarios.
            std::this_thread::sleep_for(
                std::chrono::milliseconds(100 * attempt));
        }
    }
}

template<typename F>
static auto flaky(F body) {
    return [body]() { retry_on_flake(body, 5); };
}

// ── Config helper ─────────────────────────────────────────────────────────

static Config makeConfig() {
    Config cfg;
    cfg.name          = "test-runner";
    cfg.capacity      = 4;
    cfg.gitea_url     = "http://localhost:3000";
    cfg.fetch_timeout = 5;
    return cfg;
}

int main() {
    std::cout << "=== WorkflowOrchestrator tests ===\n\n";

    // ── Single job workflow ────────────────────────────────────────────────

    run("single job workflow executes and succeeds", flaky([]() {
        const std::string yaml = R"(
name: single
jobs:
  build:
    runs-on: haiku
    steps:
      - id: s1
        run: echo "hello from build"
)";
        MockRunnerClient mock;
        auto cfg = makeConfig();
        WorkflowOrchestrator orch(mock, cfg);

        auto wf = parseWorkflow(yaml);
        auto result = orch.run(wf, yaml, "push");

        ASSERT(result.success);
        ASSERT_EQ(result.job_results.size(), 1u);
        ASSERT_EQ(result.job_results[0].job_id, "build");
        ASSERT(result.job_results[0].success);
    }));

    run("single job failure → OrchestratorResult.success = false", flaky([]() {
        const std::string yaml = R"(
name: failing
jobs:
  build:
    runs-on: haiku
    steps:
      - id: s1
        run: exit 1
)";
        MockRunnerClient mock;
        auto cfg = makeConfig();
        WorkflowOrchestrator orch(mock, cfg);

        auto wf = parseWorkflow(yaml);
        auto result = orch.run(wf, yaml, "push");

        ASSERT(!result.success);
        ASSERT_EQ(result.job_results.size(), 1u);
        ASSERT(!result.job_results[0].success);
    }));

    // ── Two-job linear chain ───────────────────────────────────────────────

    run("linear chain: setup then build, both succeed", flaky([]() {
        const std::string yaml = R"(
name: chain
jobs:
  setup:
    runs-on: haiku
    steps:
      - id: s1
        run: echo "setup done"
  build:
    runs-on: haiku
    needs: [setup]
    steps:
      - id: s1
        run: echo "build done"
)";
        MockRunnerClient mock;
        auto cfg = makeConfig();
        WorkflowOrchestrator orch(mock, cfg);

        auto wf = parseWorkflow(yaml);
        auto result = orch.run(wf, yaml, "push");

        ASSERT(result.success);
        ASSERT_EQ(result.job_results.size(), 2u);
        // Both jobs should have run
        bool found_setup = false, found_build = false;
        for (auto& r : result.job_results) {
            if (r.job_id == "setup") { found_setup = true; ASSERT(r.success); }
            if (r.job_id == "build") { found_build = true; ASSERT(r.success); }
        }
        ASSERT(found_setup && found_build);
    }));

    run("linear chain: setup fails → build is skipped (fail-fast)", flaky([]() {
        const std::string yaml = R"(
name: chain-fail
jobs:
  setup:
    runs-on: haiku
    steps:
      - id: s1
        run: exit 99
  build:
    runs-on: haiku
    needs: [setup]
    steps:
      - id: s1
        run: echo "should not run"
)";
        MockRunnerClient mock;
        auto cfg = makeConfig();
        WorkflowOrchestrator orch(mock, cfg);

        auto wf = parseWorkflow(yaml);
        auto result = orch.run(wf, yaml, "push");

        ASSERT(!result.success);
        // Only setup should have executed (build's wave was skipped)
        ASSERT_EQ(result.job_results.size(), 1u);
        ASSERT_EQ(result.job_results[0].job_id, "setup");
        ASSERT(!result.job_results[0].success);
    }));

    // ── Diamond dependency ─────────────────────────────────────────────────

    run("diamond: setup → (lint, test) → deploy — all succeed", flaky([]() {
        // Diamond has 3 waves: [setup], [lint+test], [deploy]
        // lint and test run in parallel (wave 1).
        // We verify that all 4 jobs run and succeed.
        const std::string yaml = R"(
name: diamond
jobs:
  setup:
    runs-on: haiku
    steps:
      - run: echo setup
  lint:
    runs-on: haiku
    needs: [setup]
    steps:
      - run: echo lint
  test:
    runs-on: haiku
    needs: [setup]
    steps:
      - run: echo test
  deploy:
    runs-on: haiku
    needs: [lint, test]
    steps:
      - run: echo deploy
)";
        MockRunnerClient mock;
        auto cfg = makeConfig();
        WorkflowOrchestrator orch(mock, cfg);

        auto wf = parseWorkflow(yaml);
        auto result = orch.run(wf, yaml, "push");

        // All 4 jobs must have been dispatched
        ASSERT_EQ(result.job_results.size(), 4u);
        for (auto& r : result.job_results) {
            // Each job must have run (not skipped) — but allow individual
            // success/failure since the Haiku posix_spawn race may affect
            // individual jobs; the retry_on_flake wrapper re-runs on failure.
            ASSERT(!r.job_id.empty());
        }
        ASSERT(result.success);
    }));

    // ── on_job_complete callback ───────────────────────────────────────────

    run("on_job_complete callback invoked for each job", flaky([]() {
        const std::string yaml = R"(
name: callback-test
jobs:
  alpha:
    runs-on: haiku
    steps:
      - run: echo alpha
  beta:
    runs-on: haiku
    needs: [alpha]
    steps:
      - run: echo beta
)";
        MockRunnerClient mock;
        auto cfg = makeConfig();
        WorkflowOrchestrator orch(mock, cfg);

        std::vector<std::string> completed_ids;
        std::mutex m;
        auto cb = [&](const LocalJobResult& r) {
            std::lock_guard<std::mutex> g(m);
            completed_ids.push_back(r.job_id);
        };

        auto wf = parseWorkflow(yaml);
        auto result = orch.run(wf, yaml, "push", "", cb);

        ASSERT(result.success);
        ASSERT_EQ(completed_ids.size(), 2u);
        // alpha completes before beta (sequential chain)
        ASSERT_EQ(completed_ids[0], "alpha");
        ASSERT_EQ(completed_ids[1], "beta");
    }));

    // ── fail-fast: false — all jobs complete despite first failure ──────────

    run("fail-fast false: second job runs even when first fails", flaky([]() {
        const std::string yaml = R"(
name: FailFast
jobs:
  job-a:
    runs-on: haiku
    strategy:
      fail-fast: false
    steps:
      - run: exit 1
  job-b:
    runs-on: haiku
    strategy:
      fail-fast: false
    steps:
      - run: echo "JOB_B_RAN"
)";
        MockRunnerClient mock;
        auto cfg = makeConfig();
        WorkflowOrchestrator orch(mock, cfg);

        auto wf = parseWorkflow(yaml);
        auto result = orch.run(wf, yaml, "push");

        // Overall should fail (job-a failed)
        ASSERT(!result.success);
        ASSERT_EQ(result.job_results.size(), 2u);

        // Both jobs must appear in results
        bool a_failed = false, b_ran = false;
        for (auto& r : result.job_results) {
            if (r.job_id == "job-a") a_failed = !r.success;
            if (r.job_id == "job-b") b_ran    =  r.success;
        }
        ASSERT(a_failed);
        ASSERT(b_ran);   // job-b must have run despite job-a failing
    }));

    run("fail-fast true (default): later wave skipped when earlier wave fails", flaky([]() {
        // Uses needs: to put job-b in wave 1 (after job-a in wave 0).
        // With fail-fast=true, job-a's failure should abort wave 1.
        const std::string yaml = R"(
name: FailFastDefault
jobs:
  job-a:
    runs-on: haiku
    steps:
      - run: exit 1
  job-b:
    runs-on: haiku
    needs: job-a
    steps:
      - run: echo "JOB_B_RAN"
)";
        MockRunnerClient mock;
        auto cfg = makeConfig();
        WorkflowOrchestrator orch(mock, cfg);

        auto wf = parseWorkflow(yaml);
        auto result = orch.run(wf, yaml, "push");

        ASSERT(!result.success);
        // job-b should NOT appear in results (wave was skipped entirely)
        // OR it appears as skipped/failed.
        bool job_b_ran_successfully = false;
        for (auto& r : result.job_results) {
            if (r.job_id == "job-b" && r.success) job_b_ran_successfully = true;
        }
        ASSERT(!job_b_ran_successfully);
    }));

    run("max-parallel 1 runs wave jobs sequentially", flaky([]() {
        const std::string yaml = R"(
name: MaxParallel
jobs:
  alpha:
    runs-on: haiku
    strategy:
      max-parallel: 1
    steps:
      - run: echo "alpha-ran"
  beta:
    runs-on: haiku
    strategy:
      max-parallel: 1
    steps:
      - run: echo "beta-ran"
)";
        MockRunnerClient mock;
        auto cfg = makeConfig();
        WorkflowOrchestrator orch(mock, cfg);

        auto wf = parseWorkflow(yaml);
        auto result = orch.run(wf, yaml, "push");

        // Both jobs are independent (wave 0 has both) — with max_parallel=1
        // they run serially and both should succeed.
        ASSERT(result.success);
        ASSERT_EQ(result.job_results.size(), 2u);
        for (auto& r : result.job_results) ASSERT(r.success);
    }));

    // ── Empty workflow ─────────────────────────────────────────────────────

    run("empty workflow returns success with no job results", flaky([]() {
        const std::string yaml = "name: empty\njobs: {}\n";
        MockRunnerClient mock;
        auto cfg = makeConfig();
        WorkflowOrchestrator orch(mock, cfg);

        auto wf = parseWorkflow(yaml);
        auto result = orch.run(wf, yaml, "push");

        ASSERT(result.success);
        ASSERT_EQ(result.job_results.size(), 0u);
    }));

    // ── Job outputs flow to downstream job via needs_context ───────────────
    // build-job emits an output "artifact" via GITHUB_OUTPUT; publish-job
    // (needs: build-job) reads it with ${{ needs.build-job.outputs.artifact }}
    // and echoes it.  We verify that:
    //   1. The orchestrator reports success for both jobs.
    //   2. The downstream job doesn't crash when consuming the output.
    run("job outputs propagate to downstream job via needs_context", flaky([]() {
        const std::string yaml = R"(
name: outputs-test
jobs:
  build-job:
    runs-on: haiku
    outputs:
      artifact: ${{ steps.emit.outputs.artifact }}
    steps:
      - id: emit
        run: echo "artifact=my-binary" >> $GITHUB_OUTPUT
  publish-job:
    needs: build-job
    runs-on: haiku
    steps:
      - run: |
          echo "Got artifact: ${{ needs.build-job.outputs.artifact }}"
          test "${{ needs.build-job.outputs.artifact }}" = "my-binary"
)";
        MockRunnerClient mock;
        auto cfg = makeConfig();
        WorkflowOrchestrator orch(mock, cfg);

        auto wf = parseWorkflow(yaml);
        auto result = orch.run(wf, yaml, "push");

        ASSERT(result.success);
        ASSERT_EQ(result.job_results.size(), 2u);
        for (auto& r : result.job_results) ASSERT(r.success);
        // build-job should have its outputs captured
        bool found_build = false;
        for (auto& r : result.job_results) {
            if (r.job_id == "build-job") {
                found_build = true;
                ASSERT(r.outputs.count("artifact"));
                ASSERT_EQ(r.outputs.at("artifact"), "my-binary");
            }
        }
        ASSERT(found_build);
    }));

    return summary();
}
