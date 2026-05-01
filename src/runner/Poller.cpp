// Poller.cpp — FetchTask polling loop and task dispatch
#include "Poller.h"
#include "TaskExecutor.h"
#include "../util/Logger.h"

#include <chrono>

namespace runner {

Poller::Poller(IRunnerClient& client, const Config& cfg, const RunnerState& state)
    : client_(client)
    , cfg_(cfg)
    , state_(state)
    , capacity_sem_(cfg.capacity > 0 && cfg.capacity <= 16
                    ? cfg.capacity : 1)
{}

Poller::~Poller() {
    stop();
    join();
}

// ─── start ────────────────────────────────────────────────────────────────

void Poller::start() {
    running_.store(true);
    shutdown_.store(false);
    poll_thread_ = std::thread([this]() { pollLoop(); });
    ping_thread_ = std::thread([this]() { pingLoop(); });
}

// ─── stop / join ──────────────────────────────────────────────────────────

void Poller::stop() {
    shutdown_.store(true);
}

void Poller::join() {
    if (poll_thread_.joinable()) poll_thread_.join();
    if (ping_thread_.joinable()) ping_thread_.join();

    // Wait for all detached task threads to finish.
    // Use a 60-second timeout; if threads are still running after that,
    // log a warning and return anyway (prevents infinite hang on SIGKILL).
    std::unique_lock<std::mutex> lock(active_mutex_);
    bool all_done = active_cv_.wait_for(
        lock,
        std::chrono::seconds(60),
        [this]() { return active_tasks_.load() == 0; });

    if (!all_done) {
        LOG_WARN("Poller", "join() timed out with "
                 << active_tasks_.load() << " task(s) still running — proceeding");
    }

    running_.store(false);
}

// ─── pingLoop ─────────────────────────────────────────────────────────────

void Poller::pingLoop() {
    while (!shutdown_.load()) {
        // Sleep in 1-second chunks so we respond to shutdown quickly
        for (int i = 0; i < 30 && !shutdown_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (shutdown_.load()) break;

        try {
            client_.ping(cfg_.name);
            LOG_DEBUG("Poller", "Ping OK");
        } catch (const std::exception& e) {
            LOG_WARN("Poller", "Ping failed: " << e.what());
        }
    }
    LOG_DEBUG("Poller", "Ping loop exiting");
}

// ─── pollLoop ─────────────────────────────────────────────────────────────

void Poller::pollLoop() {
    const std::vector<std::string>& labels = state_.labels;
    LOG_INFO("Poller", "Poll loop started  labels="
             << [&]{ std::string s; for(auto& l:labels){s+=l+' ';}; return s; }());

    while (!shutdown_.load()) {
        FetchTaskResult result;
        try {
            result = client_.fetchTask(
                labels,
                tasks_version_,
                cfg_.fetch_timeout + 5   // slightly longer than server window
            );
        } catch (const std::exception& e) {
            if (shutdown_.load()) break;
            LOG_WARN("Poller", "FetchTask error: " << e.what());
            std::this_thread::sleep_for(
                std::chrono::seconds(cfg_.fetch_interval));
            continue;
        }

        tasks_version_ = result.tasks_version;

        if (!result.task.has_value()) {
            // Nothing to do — short sleep then poll again
            std::this_thread::sleep_for(
                std::chrono::seconds(cfg_.fetch_interval));
            continue;
        }

        LOG_INFO("Poller", "Task " << result.task->id << " received"
                 << "  version=" << tasks_version_);

        // Acquire a capacity slot (blocks if all slots busy).
        // Poll shutdown every second so we don't get stuck.
        while (!shutdown_.load()) {
            if (capacity_sem_.try_acquire_for(std::chrono::seconds(1))) break;
        }
        if (shutdown_.load()) break;

        dispatchTask(std::move(*result.task));
    }

    LOG_INFO("Poller", "Poll loop exiting");
}

// ─── dispatchTask ─────────────────────────────────────────────────────────

void Poller::dispatchTask(TaskDto task) {
    active_tasks_.fetch_add(1, std::memory_order_relaxed);

    // Detach the thread — lifecycle tracked via active_tasks_ counter.
    // RAII guard ensures capacity_sem_.release() and the counter decrement
    // always happen even if TaskExecutor throws or the thread is killed.
    std::thread([this, task = std::move(task)]() mutable {
        LOG_INFO("Poller", "Dispatching task " << task.id);

        // RAII guard: always release capacity + decrement counter on exit.
        struct Guard {
            Poller* p;
            ~Guard() {
                p->capacity_sem_.release();
                int remaining = p->active_tasks_.fetch_sub(
                    1, std::memory_order_acq_rel) - 1;
                LOG_DEBUG("Poller", "Task thread finished  active=" << remaining);
                if (remaining == 0) {
                    std::lock_guard<std::mutex> lk(p->active_mutex_);
                    p->active_cv_.notify_all();
                }
            }
        } guard{this};

        try {
            TaskExecutor executor(client_, std::move(task), cfg_);
            executor.execute();
        } catch (const std::exception& e) {
            LOG_ERROR("Poller", "TaskExecutor threw: " << e.what());
        } catch (...) {
            LOG_ERROR("Poller", "TaskExecutor threw unknown exception");
        }
        // guard destructor runs here, always releasing the slot.
    }).detach();
}

} // namespace runner
