// Poller.cpp — FetchTask polling loop and task dispatch
//
// Task threads are managed via a LONG-LIVED THREAD POOL instead of
// per-task detached threads.  This is required on Haiku to avoid a crash
// in libcurl/OpenSSL TLS (thread-local storage) destructors that fire when
// a short-lived detached thread exits — a known Haiku TLS destructor race
// with detached pthreads.  Long-lived pool threads create their TLS state
// once (on first curl call) and only tear it down when the pool shuts down.

#include "Poller.h"
#include "TaskExecutor.h"
#include "../util/Logger.h"

#include <pthread.h>
#include <signal.h>
#include <setjmp.h>
#include <cstring>
#include <chrono>
#include <optional>

// Thread-local signal recovery — used by benign_signal_handler() in main.cpp
// to jump out of a faulting task back to the workerLoop boundary.
// Defined here so they're available in test builds that don't link main.cpp.
thread_local sigjmp_buf t_task_recover;
thread_local volatile bool t_in_task = false;

namespace runner {

// ─── constructor / destructor ─────────────────────────────────────────────

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

    // Spawn the long-lived worker pool threads BEFORE the poll/ping threads
    // so they're ready to receive tasks immediately.
    int pool_size = (cfg_.capacity > 0 && cfg_.capacity <= 16) ? cfg_.capacity : 1;
    for (int i = 0; i < pool_size; ++i) {
        workers_.emplace_back([this]() {
#ifdef __HAIKU__
            // Ignore SIGKILLTHR in every pool thread — same as the main thread.
            // SIG_IGN is set at the process level in main(), but threads inherit
            // the signal mask (not the disposition), so we set it here as well.
            struct sigaction ign{};
            ign.sa_handler = SIG_IGN;
            sigemptyset(&ign.sa_mask);
            sigaction(SIGKILLTHR, &ign, nullptr);
#endif
            workerLoop();
        });
    }

    poll_thread_ = std::thread([this]() {
#ifdef __HAIKU__
        struct sigaction ign{}; ign.sa_handler = SIG_IGN;
        sigemptyset(&ign.sa_mask); sigaction(SIGKILLTHR, &ign, nullptr);
#endif
        pollLoop();
    });
    ping_thread_ = std::thread([this]() {
#ifdef __HAIKU__
        struct sigaction ign{}; ign.sa_handler = SIG_IGN;
        sigemptyset(&ign.sa_mask); sigaction(SIGKILLTHR, &ign, nullptr);
#endif
        pingLoop();
    });
}

// ─── stop / join ──────────────────────────────────────────────────────────

void Poller::stop() {
    shutdown_.store(true);
    // Wake all pool workers so they can see shutdown_ and exit.
    work_cv_.notify_all();
}

void Poller::join() {
    if (poll_thread_.joinable()) poll_thread_.join();
    if (ping_thread_.joinable()) ping_thread_.join();

    // Wait for in-flight tasks to finish (up to 60 s).
    {
        std::unique_lock<std::mutex> lock(active_mutex_);
        bool all_done = active_cv_.wait_for(
            lock, std::chrono::seconds(60),
            [this]() { return active_tasks_.load() == 0; });
        if (!all_done) {
            LOG_WARN("Poller", "join() timed out with "
                     << active_tasks_.load() << " task(s) still running — proceeding");
        }
    }

    // Ensure shutdown_ is true so workers see it in their wait predicate,
    // then drain any remaining work-queue items and wake all workers so
    // they can observe shutdown_ and exit cleanly.
    shutdown_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(work_mutex_);
        while (!work_queue_.empty()) work_queue_.pop();
    }
    work_cv_.notify_all();

    // Join all pool workers (they're joinable, not detached).
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();

    running_.store(false);
}

// ─── workerLoop ───────────────────────────────────────────────────────────
// Each pool thread blocks here waiting for work items.

void Poller::workerLoop() {
    while (true) {
        std::optional<WorkItem> item;

        {
            std::unique_lock<std::mutex> lk(work_mutex_);
            work_cv_.wait(lk, [this]() {
                return shutdown_.load() || !work_queue_.empty();
            });
            if (shutdown_.load() && work_queue_.empty()) break;
            if (!work_queue_.empty()) {
                item = std::move(work_queue_.front());
                work_queue_.pop();
            }
        }

        if (!item.has_value()) continue;

        // ── Execute the task ─────────────────────────────────────────
        LOG_INFO("Poller", "Dispatching task " << item->task.id);

        active_tasks_.fetch_add(1, std::memory_order_acquire);

        // Set the recovery point for SIGSEGV/SIGBUS inside TaskExecutor.
        // If a signal fires, execution resumes here with a non-zero value.
        int sig_caught = sigsetjmp(t_task_recover, 1);
        if (sig_caught == 0) {
            // Normal path: execute the task.
            t_in_task = true;
            try {
                TaskExecutor executor(client_, std::move(item->task), cfg_);
                executor.execute();
            } catch (const std::exception& e) {
                LOG_ERROR("Poller", "TaskExecutor threw: " << e.what());
            } catch (...) {
                LOG_ERROR("Poller", "TaskExecutor threw unknown exception");
            }
            t_in_task = false;
        } else {
            // SIGSEGV or SIGBUS was caught — log and continue.
            t_in_task = false;
            LOG_ERROR("Poller", "Task " << item->task.id
                      << " aborted by signal " << sig_caught
                      << " — task failed, daemon continues");
        }

        // Reset tasks_version_ to 0 so the next FetchTask call is a
        // "give me any pending task" poll rather than a long-poll waiting
        // for a version bump.  This ensures dependent jobs (needs:) are
        // picked up immediately after the upstream job completes.
        // Use release ordering so the reset is visible before capacity is
        // released and the poll loop fetches again.
        tasks_version_.store(0, std::memory_order_release);
        capacity_sem_.release();
        int remaining = active_tasks_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        LOG_DEBUG("Poller", "Task finished  active=" << remaining);
        if (remaining == 0) {
            std::lock_guard<std::mutex> lk(active_mutex_);
            active_cv_.notify_all();
        }
    }
    LOG_DEBUG("Poller", "Worker thread exiting");
}

// ─── pingLoop ─────────────────────────────────────────────────────────────

void Poller::pingLoop() {
    while (!shutdown_.load()) {
        for (int i = 0; i < 30 && !shutdown_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (shutdown_.load()) break;

        try {
            client_.ping(cfg_.name);
            LOG_INFO("Poller", "Ping OK");
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
                cfg_.fetch_timeout + 5
            );
        } catch (const std::exception& e) {
            if (shutdown_.load()) break;
            LOG_WARN("Poller", "FetchTask error: " << e.what());
            std::this_thread::sleep_for(std::chrono::seconds(cfg_.fetch_interval));
            continue;
        }

        tasks_version_ = result.tasks_version;

        if (!result.task.has_value()) {
            LOG_DEBUG("Poller", "FetchTask: no task  tasks_version=" << tasks_version_);
            std::this_thread::sleep_for(std::chrono::seconds(cfg_.fetch_interval));
            continue;
        }

        LOG_INFO("Poller", "Task " << result.task->id << " received"
                 << "  version=" << tasks_version_);

        // Acquire a capacity slot (blocks if all slots busy).
        while (!shutdown_.load()) {
            if (capacity_sem_.try_acquire_for(std::chrono::seconds(1))) break;
        }
        if (shutdown_.load()) break;

        dispatchTask(std::move(*result.task));
    }

    LOG_INFO("Poller", "Poll loop exiting");
}

// ─── dispatchTask ─────────────────────────────────────────────────────────
// Enqueue the task for a pool worker to pick up.

void Poller::dispatchTask(TaskDto task) {
    std::lock_guard<std::mutex> lk(work_mutex_);
    work_queue_.push(WorkItem{std::move(task)});
    work_cv_.notify_one();
}

} // namespace runner
