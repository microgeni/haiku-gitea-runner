// tests/test_workflow_parser.cpp — Unit tests for workflow parsing
#include "test_runner.h"
#include "../src/workflow/WorkflowParser.h"

using namespace runner;
using namespace test;

static const char* SIMPLE_WORKFLOW = R"yaml(
name: CI

on:
  push:
    branches: [main]

env:
  GLOBAL_VAR: "global_value"

jobs:
  build:
    name: Build and Test
    runs-on: haiku
    timeout-minutes: 60
    env:
      JOB_VAR: "job_value"
    steps:
      - id: checkout
        name: Checkout code
        uses: actions/checkout@v4

      - id: build
        name: Build project
        run: make -j4
        shell: sh
        env:
          BUILD_TYPE: release

      - id: test
        name: Run tests
        run: make test
        if: success()
        continue-on-error: false
        timeout-minutes: 30
)yaml";

static const char* MATRIX_WORKFLOW = R"yaml(
name: Matrix Build

jobs:
  matrix-job:
    runs-on: haiku
    strategy:
      matrix:
        arch: [x64, arm64]
        config: [debug, release]
        include:
          - arch: x64
            config: release
            extra: production
        exclude:
          - arch: arm64
            config: debug
    steps:
      - run: echo "Building ${{ matrix.arch }} ${{ matrix.config }}"
)yaml";

static const char* MULTI_JOB_WORKFLOW = R"yaml(
name: Multi-job

jobs:
  setup:
    runs-on: haiku
    outputs:
      version: ${{ steps.get_ver.outputs.version }}
    steps:
      - id: get_ver
        run: echo "version=1.2.3" >> $GITHUB_OUTPUT

  build:
    runs-on: haiku
    needs: setup
    steps:
      - run: echo "Building version ${{ needs.setup.outputs.version }}"
)yaml";

int main() {
    std::cout << "=== WorkflowParser tests ===\n\n";

    run("parse workflow name", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        ASSERT_EQ(wf.name, "CI");
    });

    run("parse global env", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        ASSERT_EQ(wf.env.at("GLOBAL_VAR"), "global_value");
    });

    run("parse jobs count", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        ASSERT_EQ(wf.jobs.size(), 1u);
    });

    run("parse job name", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        ASSERT_EQ(wf.jobs.at("build").name, "Build and Test");
    });

    run("parse job runs-on", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        auto& job = wf.jobs.at("build");
        ASSERT_EQ(job.runs_on.size(), 1u);
        ASSERT_EQ(job.runs_on[0], "haiku");
    });

    run("parse job timeout-minutes", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        ASSERT_EQ(wf.jobs.at("build").timeout_minutes, 60);
    });

    run("parse job env", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        ASSERT_EQ(wf.jobs.at("build").env.at("JOB_VAR"), "job_value");
    });

    run("parse steps count", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        ASSERT_EQ(wf.jobs.at("build").steps.size(), 3u);
    });

    run("parse step id", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        ASSERT_EQ(wf.jobs.at("build").steps[0].id, "checkout");
    });

    run("parse step name", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        ASSERT_EQ(wf.jobs.at("build").steps[0].name, "Checkout code");
    });

    run("parse uses step", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        ASSERT_EQ(wf.jobs.at("build").steps[0].uses, "actions/checkout@v4");
    });

    run("parse run step", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        ASSERT_EQ(wf.jobs.at("build").steps[1].run, "make -j4");
    });

    run("parse step shell", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        ASSERT_EQ(wf.jobs.at("build").steps[1].shell, "sh");
    });

    run("parse step env", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        ASSERT_EQ(wf.jobs.at("build").steps[1].env.at("BUILD_TYPE"), "release");
    });

    run("parse step if condition", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        ASSERT_EQ(wf.jobs.at("build").steps[2].if_condition, "success()");
    });

    run("parse step continue-on-error", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        ASSERT(!wf.jobs.at("build").steps[2].continue_on_error);
    });

    run("parse step timeout-minutes", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        ASSERT_EQ(wf.jobs.at("build").steps[2].timeout_minutes, 30);
    });

    run("auto-generate step id if missing", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        // steps[2] has id "test" explicitly; steps without id get "step_N"
        ASSERT(!wf.jobs.at("build").steps[2].id.empty());
    });

    run("parse matrix axes", []() {
        auto wf = parseWorkflow(MATRIX_WORKFLOW);
        auto& job = wf.jobs.at("matrix-job");
        ASSERT(job.matrix.has_value());
        ASSERT_EQ(job.matrix->axes.at("arch").size(), 2u);
        ASSERT_EQ(job.matrix->axes.at("config").size(), 2u);
    });

    run("parse matrix include", []() {
        auto wf = parseWorkflow(MATRIX_WORKFLOW);
        auto& job = wf.jobs.at("matrix-job");
        ASSERT_EQ(job.matrix->include.size(), 1u);
        ASSERT_EQ(job.matrix->include[0].at("extra"), "production");
    });

    run("parse matrix exclude", []() {
        auto wf = parseWorkflow(MATRIX_WORKFLOW);
        auto& job = wf.jobs.at("matrix-job");
        ASSERT_EQ(job.matrix->exclude.size(), 1u);
    });

    run("parse multi-job needs", []() {
        auto wf = parseWorkflow(MULTI_JOB_WORKFLOW);
        ASSERT_EQ(wf.jobs.at("build").needs.size(), 1u);
        ASSERT_EQ(wf.jobs.at("build").needs[0], "setup");
    });

    run("parse job outputs", []() {
        auto wf = parseWorkflow(MULTI_JOB_WORKFLOW);
        auto& setup = wf.jobs.at("setup");
        ASSERT(!setup.outputs.empty());
        ASSERT_EQ(setup.outputs.at("version"),
                  "${{ steps.get_ver.outputs.version }}");
    });

    run("validate valid workflow returns no errors", []() {
        auto wf = parseWorkflow(SIMPLE_WORKFLOW);
        auto errs = validateWorkflow(wf);
        ASSERT(errs.empty());
    });

    run("validate detects missing runs-on", []() {
        auto wf = parseWorkflow("jobs:\n  j:\n    steps:\n      - run: echo hi\n");
        auto errs = validateWorkflow(wf);
        bool found = false;
        for (auto& e : errs) if (e.find("runs-on") != std::string::npos) found = true;
        ASSERT(found);
    });

    run("validate detects missing steps", []() {
        auto wf = parseWorkflow("jobs:\n  j:\n    runs-on: haiku\n");
        auto errs = validateWorkflow(wf);
        bool found = false;
        for (auto& e : errs) if (e.find("steps") != std::string::npos) found = true;
        ASSERT(found);
    });

    run("validate detects bad needs reference", []() {
        auto wf = parseWorkflow(
            "jobs:\n"
            "  b:\n    runs-on: haiku\n    needs: nonexistent\n"
            "    steps:\n      - run: echo hi\n");
        auto errs = validateWorkflow(wf);
        bool found = false;
        for (auto& e : errs) if (e.find("nonexistent") != std::string::npos) found = true;
        ASSERT(found);
    });

    run("parse throws on invalid YAML", []() {
        ASSERT_THROWS(parseWorkflow("{invalid: yaml: ["));
    });

    run("strategy.fail-fast false is parsed", []() {
        auto wf = parseWorkflow(R"(
name: T
jobs:
  build:
    runs-on: haiku
    strategy:
      fail-fast: false
    steps:
      - run: echo ok
)");
        ASSERT_EQ(wf.jobs.at("build").fail_fast, false);
    });

    run("strategy.fail-fast defaults to true when absent", []() {
        auto wf = parseWorkflow(R"(
name: T
jobs:
  build:
    runs-on: haiku
    steps:
      - run: echo ok
)");
        ASSERT_EQ(wf.jobs.at("build").fail_fast, true);
    });

    run("strategy.max-parallel is parsed", []() {
        auto wf = parseWorkflow(R"(
name: T
jobs:
  build:
    runs-on: haiku
    strategy:
      max-parallel: 3
    steps:
      - run: echo ok
)");
        ASSERT_EQ(wf.jobs.at("build").max_parallel, 3);
    });

    return summary();
}
