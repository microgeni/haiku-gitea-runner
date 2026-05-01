#pragma once
// Poller.h — Main FetchTask polling loop
//
// Runs two background threads:
//   poll_thread_: calls FetchTask repeatedly; dispatches tasks
//   ping_thread_: periodic keepalive Ping every 30 s
//
// Task threads are DETACHED after dispatch.  An atomic counter tracks
// the number of in-flight tasks; join() waits for it to reach zero.
// The capacity counting_semaphore limits concurrent task count cleanly.

#include "../client/IRunnerClient.h"
#include "../config/Config.h"
#include "../config/RunnerState.h"

#include <atomic>
#include <semaphore>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace runner {

class Poller {
public:
    Poller(IRunnerClient&     client,
           const Config&      cfg,
           const RunnerState& state);

    ~Poller();

    Poller(const Poller&)            = delete;
    Poller& operator=(const Poller&) = delete;

    /// Start poll + ping background threads.
    void start();

    /// Signal graceful shutdown (non-blocking).
    void stop();

    /// Block until poll/ping threads exit and all in-flight tasks complete.
    void join();

    bool running() const { return running_.load(); }

    /// Number of tasks currently executing.
    int activeTaskCount() const { return active_tasks_.load(); }

private:
    IRunnerClient&      client_;
    const Config&       cfg_;
    const RunnerState&  state_;

    std::atomic<bool>   running_{false};
    std::atomic<bool>   shutdown_{false};

    std::thread         poll_thread_;
    std::thread         ping_thread_;

    // Capacity: blocks dispatch when all slots are in use.
    // Max template argument must be a compile-time constant ≥ cfg_.capacity.
    std::counting_semaphore<16> capacity_sem_;

    // Counts detached task threads currently alive.
    std::atomic<int>            active_tasks_{0};
    std::mutex                  active_mutex_;
    std::condition_variable     active_cv_;   // notified when active_tasks_ → 0

    int64_t tasks_version_ = 0;

    void pollLoop();
    void pingLoop();

    /// Detach a task thread; active_tasks_ is incremented before detach,
    /// decremented (and active_cv_ notified) when the thread body returns.
    void dispatchTask(TaskDto task);
};

} // namespace runner
