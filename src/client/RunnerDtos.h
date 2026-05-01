#pragma once
// RunnerDtos.h — Plain-old-data types mirroring the Gitea RunnerService
// protobuf messages.
//
// Extracted into a standalone header so that both RunnerClient (the
// concrete Connect-RPC implementation) and IRunnerClient (the abstract
// interface used for testing) can reference these types without creating
// a header-include cycle.

#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include <utility>

namespace runner {

struct PingResult {
    std::string data;
};

struct RegisterResult {
    std::string runner_token;
    std::string uuid;
    std::string name;
    std::vector<std::string> labels;
};

struct StepContextDto {
    std::string id;
    int result = 0;  // maps to Result enum (1=success, 2=failure, ...)
    std::vector<std::pair<std::string,std::string>> outputs;
};

struct TaskDto {
    int64_t id = 0;
    std::string workflow_payload;   // raw YAML bytes
    std::vector<std::pair<std::string,std::string>> context;   // github.* context
    std::vector<std::pair<std::string,std::string>> secrets;
    std::vector<std::pair<std::string,std::string>> vars;
    std::string gitea_runtime_token;
    std::vector<StepContextDto> needs_context;
    int64_t timeout = 0;   // seconds
    std::string machine;
};

struct FetchTaskResult {
    std::optional<TaskDto> task;
    int64_t tasks_version = 0;
};

struct StepStateDto {
    int64_t id = 0;
    int result = 0;        // Result enum
    int64_t started_at_s  = 0;  // unix seconds
    int64_t stopped_at_s  = 0;
    int64_t log_index  = 0;
    int64_t log_length = 0;
};

struct UpdateTaskResult {
    int64_t task_id = 0;
    std::string state;
};

struct LogRowDto {
    int64_t     time_s   = 0;   // unix seconds
    int32_t     time_ns  = 0;   // nanoseconds part
    std::string content;
};

struct UpdateLogResult {
    int64_t ack_index = 0;
};

} // namespace runner
