#pragma once
// JobGraph.h — Topological ordering of workflow jobs by needs: dependencies
//
// Builds a DAG from job.needs[] declarations and returns jobs in
// dependency-first order (Kahn's algorithm).  Detects cycles.

#include "../workflow/WorkflowParser.h"

#include <string>
#include <vector>
#include <map>
#include <stdexcept>

namespace runner {

/// Result of topological sort: ordered job IDs, or error.
struct JobOrderResult {
    std::vector<std::string> order;   ///< job IDs in execution order
    bool                     has_cycle = false;
    std::string              cycle_description;
};

/// Compute the topological execution order for all jobs in a workflow.
/// Jobs with no needs: come first; jobs that need others come after their
/// dependencies are satisfied.
///
/// @throws std::runtime_error if any needs: reference doesn't exist
JobOrderResult topologicalJobOrder(const std::map<std::string, Job>& jobs);

/// Given the full job map and the topological order, return which jobs
/// can run concurrently at each "wave" (all dependencies of a wave's jobs
/// are completed by prior waves).
///
/// E.g. if the order is [setup, build, test, deploy]:
///   wave 0: [setup]
///   wave 1: [build]  (needs setup)
///   wave 2: [test]   (needs build)
///   wave 3: [deploy] (needs test)
/// or if build and lint are independent:
///   wave 0: [setup]
///   wave 1: [build, lint]  (both need setup only)
///   wave 2: [test]         (needs build)
std::vector<std::vector<std::string>> waveSchedule(
    const std::map<std::string, Job>& jobs,
    const std::vector<std::string>&   order);

} // namespace runner
