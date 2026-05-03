#pragma once
// IRunnerClient.h — Abstract interface for the Gitea RunnerService client.
//
// Extracted to allow mock implementations for unit/integration testing
// without requiring a live server or libcurl.
//
// RunnerClient implements this interface; MockRunnerClient (in tests/) also
// implements it for testing TaskExecutor/Poller in isolation.

#include "RunnerDtos.h"   // for the DTOs (PingResult, TaskDto, etc.)

namespace runner {

/// Pure-virtual interface for the 5 RunnerService RPCs.
class IRunnerClient {
public:
    virtual ~IRunnerClient() = default;

    virtual void setRunnerToken(std::string token) = 0;

    virtual PingResult ping(const std::string& data = "") = 0;

    virtual RegisterResult registerRunner(
        const std::string& reg_token,
        const std::string& name,
        const std::vector<std::string>& labels,
        const std::string& os      = "haiku",
        const std::string& arch    = "x86_64",
        const std::string& version = "0.1.0-haiku"
    ) = 0;

    /// Declare the runner's version and labels to Gitea after registration.
    /// Gitea 1.21+ expects a Declare call immediately after Register so it
    /// can store the runner's label list server-side for task routing.
    virtual RegisterResult declare(
        const std::vector<std::string>& labels,
        const std::string& version = "0.1.0-haiku"
    ) = 0;

    virtual FetchTaskResult fetchTask(
        const std::vector<std::string>& labels,
        int64_t tasks_version,
        int     timeout_s = 60
    ) = 0;

    virtual UpdateTaskResult updateTask(
        int64_t task_id,
        int     state,
        const std::vector<StepStateDto>& steps,
        int64_t started_at_s = 0,
        int64_t stopped_at_s = 0,
        const std::vector<std::pair<std::string,std::string>>& outputs = {}
    ) = 0;

    virtual UpdateLogResult updateLog(
        int64_t task_id,
        int64_t index,
        const std::vector<LogRowDto>& rows,
        bool no_more = false
    ) = 0;
};

} // namespace runner
