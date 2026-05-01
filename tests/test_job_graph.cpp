// tests/test_job_graph.cpp — Unit tests for JobGraph (topological sort + wave schedule)
#include "test_runner.h"
#include "../src/runner/JobGraph.h"
#include "../src/workflow/WorkflowParser.h"

using namespace test;
using namespace runner;

// Helper: build a minimal Job with given id and needs list
static Job makeJob(const std::string& id, std::vector<std::string> needs = {}) {
    Job j;
    j.id    = id;
    j.name  = id;
    j.runs_on = {"haiku"};
    j.needs = std::move(needs);
    // Add one dummy step to make it non-empty
    Step s;
    s.id  = "step1";
    s.run = "echo hello";
    j.steps.push_back(s);
    return j;
}

int main() {
    std::cout << "=== JobGraph tests ===\n\n";

    // ── topologicalJobOrder ───────────────────────────────────────────────

    run("empty workflow returns empty order", []() {
        std::map<std::string, Job> jobs;
        auto result = topologicalJobOrder(jobs);
        ASSERT(!result.has_cycle);
        ASSERT(result.order.empty());
    });

    run("single job, no needs", []() {
        std::map<std::string, Job> jobs;
        jobs["build"] = makeJob("build");
        auto result = topologicalJobOrder(jobs);
        ASSERT(!result.has_cycle);
        ASSERT_EQ(result.order.size(), 1u);
        ASSERT_EQ(result.order[0], "build");
    });

    run("two independent jobs both appear", []() {
        std::map<std::string, Job> jobs;
        jobs["build"] = makeJob("build");
        jobs["lint"]  = makeJob("lint");
        auto result = topologicalJobOrder(jobs);
        ASSERT(!result.has_cycle);
        ASSERT_EQ(result.order.size(), 2u);
    });

    run("linear chain: setup → build → test", []() {
        std::map<std::string, Job> jobs;
        jobs["setup"] = makeJob("setup");
        jobs["build"] = makeJob("build", {"setup"});
        jobs["test"]  = makeJob("test",  {"build"});
        auto result = topologicalJobOrder(jobs);
        ASSERT(!result.has_cycle);
        ASSERT_EQ(result.order.size(), 3u);
        // setup must precede build, build must precede test
        auto pos = [&](const std::string& id) -> size_t {
            for (size_t i = 0; i < result.order.size(); ++i)
                if (result.order[i] == id) return i;
            return SIZE_MAX;
        };
        ASSERT(pos("setup") < pos("build"));
        ASSERT(pos("build") < pos("test"));
    });

    run("diamond dependency: A → B,C → D", []() {
        std::map<std::string, Job> jobs;
        jobs["A"] = makeJob("A");
        jobs["B"] = makeJob("B", {"A"});
        jobs["C"] = makeJob("C", {"A"});
        jobs["D"] = makeJob("D", {"B", "C"});
        auto result = topologicalJobOrder(jobs);
        ASSERT(!result.has_cycle);
        ASSERT_EQ(result.order.size(), 4u);
        auto pos = [&](const std::string& id) -> size_t {
            for (size_t i = 0; i < result.order.size(); ++i)
                if (result.order[i] == id) return i;
            return SIZE_MAX;
        };
        ASSERT(pos("A") < pos("B"));
        ASSERT(pos("A") < pos("C"));
        ASSERT(pos("B") < pos("D"));
        ASSERT(pos("C") < pos("D"));
    });

    run("all jobs appear in order result", []() {
        std::map<std::string, Job> jobs;
        jobs["a"] = makeJob("a");
        jobs["b"] = makeJob("b", {"a"});
        jobs["c"] = makeJob("c", {"a"});
        jobs["d"] = makeJob("d", {"b", "c"});
        auto result = topologicalJobOrder(jobs);
        ASSERT_EQ(result.order.size(), 4u);
        // All four ids present
        for (auto& id : {"a", "b", "c", "d"}) {
            bool found = false;
            for (auto& x : result.order) if (x == id) found = true;
            ASSERT(found);
        }
    });

    run("missing needs reference throws", []() {
        std::map<std::string, Job> jobs;
        jobs["build"] = makeJob("build", {"nonexistent"});
        ASSERT_THROWS(topologicalJobOrder(jobs));
    });

    run("simple cycle detected", []() {
        std::map<std::string, Job> jobs;
        jobs["A"] = makeJob("A", {"B"});
        jobs["B"] = makeJob("B", {"A"});
        auto result = topologicalJobOrder(jobs);
        ASSERT(result.has_cycle);
        ASSERT(!result.cycle_description.empty());
    });

    run("three-node cycle detected", []() {
        std::map<std::string, Job> jobs;
        jobs["A"] = makeJob("A", {"C"});
        jobs["B"] = makeJob("B", {"A"});
        jobs["C"] = makeJob("C", {"B"});
        auto result = topologicalJobOrder(jobs);
        ASSERT(result.has_cycle);
    });

    run("self-loop cycle detected (has_cycle flag set)", []() {
        std::map<std::string, Job> jobs;
        // A needs itself — not a missing reference (A exists), but a cycle
        jobs["A"] = makeJob("A", {"A"});
        auto result = topologicalJobOrder(jobs);
        // The validator passes because "A" exists; Kahn's algo detects the cycle
        ASSERT(result.has_cycle);
        ASSERT(!result.cycle_description.empty());
    });

    // ── waveSchedule ─────────────────────────────────────────────────────

    run("waveSchedule: independent jobs in wave 0", []() {
        std::map<std::string, Job> jobs;
        jobs["a"] = makeJob("a");
        jobs["b"] = makeJob("b");
        auto order = topologicalJobOrder(jobs);
        auto waves = waveSchedule(jobs, order.order);
        ASSERT_EQ(waves.size(), 1u);
        ASSERT_EQ(waves[0].size(), 2u);
    });

    run("waveSchedule: linear chain = 3 waves", []() {
        std::map<std::string, Job> jobs;
        jobs["setup"] = makeJob("setup");
        jobs["build"] = makeJob("build", {"setup"});
        jobs["test"]  = makeJob("test",  {"build"});
        auto order = topologicalJobOrder(jobs);
        ASSERT(!order.has_cycle);
        auto waves = waveSchedule(jobs, order.order);
        ASSERT_EQ(waves.size(), 3u);
        ASSERT_EQ(waves[0][0], "setup");
        ASSERT_EQ(waves[1][0], "build");
        ASSERT_EQ(waves[2][0], "test");
    });

    run("waveSchedule: diamond = 3 waves", []() {
        std::map<std::string, Job> jobs;
        jobs["A"] = makeJob("A");
        jobs["B"] = makeJob("B", {"A"});
        jobs["C"] = makeJob("C", {"A"});
        jobs["D"] = makeJob("D", {"B", "C"});
        auto order = topologicalJobOrder(jobs);
        auto waves = waveSchedule(jobs, order.order);
        ASSERT_EQ(waves.size(), 3u);
        // wave 0: A
        ASSERT_EQ(waves[0].size(), 1u);
        ASSERT_EQ(waves[0][0], "A");
        // wave 1: B and C (order within wave is preserved from topological order)
        ASSERT_EQ(waves[1].size(), 2u);
        // wave 2: D
        ASSERT_EQ(waves[2].size(), 1u);
        ASSERT_EQ(waves[2][0], "D");
    });

    run("waveSchedule: B,C in same wave are parallel candidates", []() {
        std::map<std::string, Job> jobs;
        jobs["setup"] = makeJob("setup");
        jobs["build"] = makeJob("build", {"setup"});
        jobs["lint"]  = makeJob("lint",  {"setup"});
        jobs["test"]  = makeJob("test",  {"build", "lint"});
        auto order = topologicalJobOrder(jobs);
        auto waves = waveSchedule(jobs, order.order);
        // wave 0: setup, wave 1: build+lint, wave 2: test
        ASSERT_EQ(waves.size(), 3u);
        ASSERT_EQ(waves[1].size(), 2u);
    });

    return summary();
}
