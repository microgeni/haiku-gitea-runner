// JobGraph.cpp — Kahn's algorithm topological sort for job needs:
#include "JobGraph.h"

#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>

namespace runner {

JobOrderResult topologicalJobOrder(const std::map<std::string, Job>& jobs) {
    JobOrderResult result;

    if (jobs.empty()) return result;

    // Validate all needs: references
    for (auto& [id, job] : jobs) {
        for (auto& need : job.needs) {
            if (jobs.find(need) == jobs.end()) {
                throw std::runtime_error(
                    "Job '" + id + "' needs '" + need
                    + "' which is not defined in this workflow");
            }
        }
    }

    // in-degree map: how many unresolved dependencies each job has
    std::unordered_map<std::string, int> in_degree;
    // adjacency: for each job, which jobs depend on it
    std::unordered_map<std::string, std::vector<std::string>> dependents;

    for (auto& [id, job] : jobs) {
        in_degree[id] = static_cast<int>(job.needs.size());
        for (auto& need : job.needs) {
            dependents[need].push_back(id);
        }
    }

    // Queue of jobs with no unresolved dependencies
    std::queue<std::string> ready;
    for (auto& [id, deg] : in_degree) {
        if (deg == 0) ready.push(id);
    }

    while (!ready.empty()) {
        std::string id = ready.front();
        ready.pop();
        result.order.push_back(id);

        for (auto& dep : dependents[id]) {
            if (--in_degree[dep] == 0) {
                ready.push(dep);
            }
        }
    }

    // If not all jobs were processed, there's a cycle
    if (result.order.size() != jobs.size()) {
        result.has_cycle = true;
        std::string cycle_info;
        for (auto& [id, deg] : in_degree) {
            if (deg > 0) {
                if (!cycle_info.empty()) cycle_info += ", ";
                cycle_info += id;
            }
        }
        result.cycle_description = "Cycle detected in jobs: " + cycle_info;
    }

    return result;
}

std::vector<std::vector<std::string>> waveSchedule(
    const std::map<std::string, Job>& jobs,
    const std::vector<std::string>&   order)
{
    // Assign each job a "wave" = 1 + max wave of all its needs
    std::unordered_map<std::string, int> wave;
    for (auto& id : order) {
        auto& job = jobs.at(id);
        int max_dep_wave = -1;
        for (auto& need : job.needs) {
            auto it = wave.find(need);
            if (it != wave.end()) {
                max_dep_wave = std::max(max_dep_wave, it->second);
            }
        }
        wave[id] = max_dep_wave + 1;
    }

    // Group into waves
    int max_wave = 0;
    for (auto& [id, w] : wave) max_wave = std::max(max_wave, w);

    std::vector<std::vector<std::string>> waves(max_wave + 1);
    // Preserve topological order within each wave
    for (auto& id : order) {
        waves[wave[id]].push_back(id);
    }

    return waves;
}

} // namespace runner
