// WorkflowOrchestrator.cpp — Local multi-job workflow execution
#include "WorkflowOrchestrator.h"
#include "TaskExecutor.h"
#include "../runner/JobGraph.h"
#include "../workflow/WorkflowParser.h"
#include "../util/Logger.h"

#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

namespace runner {

// ─── Constructor ──────────────────────────────────────────────────────────

WorkflowOrchestrator::WorkflowOrchestrator(IRunnerClient& client,
                                             const Config&  cfg)
    : client_(client)
    , cfg_(cfg)
{}

// ─── cancel / executor tracking ───────────────────────────────────────────

void WorkflowOrchestrator::cancel() {
    cancelled_.store(true);

    // Immediately cancel any currently-running TaskExecutors so that in-flight
    // steps (e.g. a long sleep) are killed without waiting for the current
    // step's timeout.
    std::lock_guard<std::mutex> lk(executors_mutex_);
    for (TaskExecutor* ex : active_executors_) {
        ex->cancel();
    }
}

void WorkflowOrchestrator::registerExecutor(TaskExecutor* ex) {
    std::lock_guard<std::mutex> lk(executors_mutex_);
    active_executors_.push_back(ex);

    // If we were already cancelled before this executor was registered,
    // cancel it immediately so it doesn't start any steps.
    if (cancelled_.load()) {
        ex->cancel();
    }
}

void WorkflowOrchestrator::unregisterExecutor(TaskExecutor* ex) {
    std::lock_guard<std::mutex> lk(executors_mutex_);
    active_executors_.erase(
        std::remove(active_executors_.begin(), active_executors_.end(), ex),
        active_executors_.end());
}

// ─── makeLocalTask ────────────────────────────────────────────────────────

TaskDto WorkflowOrchestrator::makeLocalTask(
    const std::string& workflow_yaml,
    const std::string& job_id,
    int64_t            task_id,
    const std::string& event_name,
    const std::string& event_payload_json,
    const std::map<std::string, LocalJobResult>& completed_jobs) const
{
    TaskDto task;
    task.id               = task_id;
    task.machine          = job_id;
    task.workflow_payload = workflow_yaml;
    task.timeout          = cfg_.fetch_timeout;

    task.context.emplace_back("event_name",  event_name);
    if (!event_payload_json.empty()) {
        task.context.emplace_back("event", event_payload_json);
    }
    task.context.emplace_back("run_id",     std::to_string(task_id));
    task.context.emplace_back("run_number", "1");
    task.context.emplace_back("actor",      cfg_.name);
    task.context.emplace_back("server_url", cfg_.gitea_url);

    // Populate needs_context from completed upstream jobs
    for (auto& [dep_id, dep_result] : completed_jobs) {
        NeedsContextEntry nc;
        nc.result = dep_result.success ? 1 /*RESULT_SUCCESS*/ : 2 /*RESULT_FAILURE*/;
        for (auto& [ok, ov] : dep_result.outputs) {
            nc.outputs.emplace_back(ok, ov);
        }
        task.needs_context.push_back({dep_id, std::move(nc)});
    }

    return task;
}

// ─── run ──────────────────────────────────────────────────────────────────

OrchestratorResult WorkflowOrchestrator::run(
    const Workflow&         wf,
    const std::string&      workflow_yaml,
    const std::string&      event_name,
    const std::string&      event_payload_json,
    JobCompleteCallback     on_job_complete)
{
    OrchestratorResult overall;

    if (wf.jobs.empty()) {
        LOG_WARN("Orchestrator", "Workflow has no jobs");
        return overall;
    }

    // ── Topological sort ──────────────────────────────────────────────────
    JobOrderResult order;
    try {
        order = topologicalJobOrder(wf.jobs);
    } catch (const std::exception& e) {
        LOG_ERROR("Orchestrator", "Job ordering failed: " << e.what());
        overall.success = false;
        return overall;
    }

    if (order.has_cycle) {
        LOG_ERROR("Orchestrator", "Cycle in workflow jobs: " << order.cycle_description);
        overall.success = false;
        return overall;
    }

    // ── Wave schedule ─────────────────────────────────────────────────────
    auto waves = waveSchedule(wf.jobs, order.order);
    LOG_INFO("Orchestrator", "Workflow '" << wf.name
             << "': " << wf.jobs.size() << " job(s) in " << waves.size() << " wave(s)");

    std::map<std::string, LocalJobResult> completed_jobs;
    static std::atomic<int64_t> task_id_seq{1};

    // ── Execute wave by wave ──────────────────────────────────────────────
    for (size_t wave_idx = 0; wave_idx < waves.size(); ++wave_idx) {
        const auto& wave = waves[wave_idx];
        if (wave.empty()) continue;

        if (cancelled_.load()) {
            LOG_INFO("Orchestrator", "Workflow cancelled before wave " << wave_idx);
            overall.success = false;
            break;
        }

        // Determine effective fail_fast and max_parallel for this wave.
        // If ANY job in the wave sets fail-fast: false, the whole wave
        // runs to completion even if a job fails.
        bool wave_fail_fast = true;
        int  wave_max_parallel = 0;  // 0 = unlimited
        for (auto& job_id : wave) {
            if (wf.jobs.count(job_id)) {
                const Job& j = wf.jobs.at(job_id);
                if (!j.fail_fast)     wave_fail_fast = false;
                if (j.max_parallel > 0 && (wave_max_parallel == 0 ||
                    j.max_parallel < wave_max_parallel))
                    wave_max_parallel = j.max_parallel;
            }
        }

        {
            std::string names;
            for (auto& j : wave) names += j + " ";
            LOG_INFO("Orchestrator", "Wave " << wave_idx << ": " << names
                     << "(fail_fast=" << wave_fail_fast
                     << ", max_parallel=" << wave_max_parallel << ")");
        }

        std::vector<LocalJobResult> wave_results(wave.size());
        std::vector<std::thread>    wave_threads;

        // If max_parallel is set, dispatch in batches of that size.
        // Otherwise dispatch all jobs in the wave at once.
        const size_t batch_size = (wave_max_parallel > 0)
                                ? static_cast<size_t>(wave_max_parallel)
                                : wave.size();

        for (size_t j_idx = 0; j_idx < wave.size(); ) {
            size_t batch_end = std::min(j_idx + batch_size, wave.size());

            for (size_t b = j_idx; b < batch_end; ++b) {
                const std::string& job_id = wave[b];

                if (!wf.jobs.count(job_id)) {
                    wave_results[b] = {job_id, false, true, {}};
                    continue;
                }

                int64_t task_id = task_id_seq.fetch_add(1);
                TaskDto task = makeLocalTask(workflow_yaml, job_id, task_id,
                                              event_name, event_payload_json,
                                              completed_jobs);

                // Stagger parallel thread launches to reduce simultaneous
                // posix_spawn calls — Haiku occasionally delivers SIGKILLTHR
                // to a spawned child when threads spawn processes simultaneously.
                if (b > j_idx) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }

                wave_threads.emplace_back(
                    [this, b, job_id,
                     task = std::move(task),
                     &wave_results,
                     &on_job_complete]() mutable
                {
                    LocalJobResult result;
                    result.job_id = job_id;

                    try {
                        TaskExecutor executor(client_, std::move(task), cfg_);

                        // Register so cancel() can reach this executor while it runs.
                        registerExecutor(&executor);
                        result.success = executor.execute();
                        unregisterExecutor(&executor);

                        // Capture job-level outputs so they can flow into
                        // downstream needs_context for the next wave.
                        for (auto& [k, v] : executor.jobOutputs()) {
                            result.outputs[k] = v;
                        }
                    } catch (const std::exception& e) {
                        LOG_ERROR("Orchestrator", "Job '" << job_id
                                  << "' threw: " << e.what());
                        result.success = false;
                    } catch (...) {
                        result.success = false;
                    }

                    wave_results[b] = result;
                    if (on_job_complete) on_job_complete(result);
                    LOG_INFO("Orchestrator", "Job '" << job_id << "': "
                             << (result.success ? "SUCCESS" : "FAILURE"));
                });
            }

            // Wait for this batch to finish before starting the next.
            for (auto& t : wave_threads) {
                if (t.joinable()) t.join();
            }
            wave_threads.clear();

            j_idx = batch_end;
        }

        bool wave_failed = false;
        for (auto& r : wave_results) {
            overall.job_results.push_back(r);
            completed_jobs[r.job_id] = r;
            if (!r.success && !r.skipped) wave_failed = true;
        }

        if (wave_failed) {
            if (wave_fail_fast) {
                LOG_WARN("Orchestrator", "Wave " << wave_idx
                         << " failed — fail-fast enabled, skipping remaining waves");
                overall.success = false;
                break;
            } else {
                LOG_INFO("Orchestrator", "Wave " << wave_idx
                         << " had failures — fail-fast disabled, continuing");
                overall.success = false;
                // Don't break — continue to next wave
            }
        }
    }

    if (overall.success) {
        overall.success = overall.allSucceeded();
    }

    LOG_INFO("Orchestrator", "Workflow '"<< wf.name << "' complete: "
             << (overall.success ? "SUCCESS" : "FAILURE"));
    return overall;
}

} // namespace runner
