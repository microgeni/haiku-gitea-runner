// RunnerState.cpp — Persistent runner identity (JSON via nlohmann/json)
#include "RunnerState.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
#include <filesystem>
#include <cstdlib>

#ifdef __HAIKU__
#  include <FindDirectory.h>
#  include <Path.h>
#endif

namespace runner {

using json = nlohmann::json;

// ─── loadRunnerState ──────────────────────────────────────────────────────

RunnerState loadRunnerState(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        // File not found → unregistered runner
        return RunnerState{};
    }

    json j;
    try {
        f >> j;
    } catch (const json::exception& e) {
        throw std::runtime_error(
            std::string("Failed to parse runner state '") + path + "': " + e.what());
    }

    RunnerState state;
    state.token = j.value("token", "");
    state.uuid  = j.value("uuid",  "");
    state.name  = j.value("name",  "");

    if (j.contains("labels") && j["labels"].is_array()) {
        for (auto& l : j["labels"]) {
            state.labels.push_back(l.get<std::string>());
        }
    }

    return state;
}

// ─── saveRunnerState ──────────────────────────────────────────────────────

void saveRunnerState(const RunnerState& state, const std::string& path) {
    json j;
    j["token"]  = state.token;
    j["uuid"]   = state.uuid;
    j["name"]   = state.name;
    j["labels"] = state.labels;

    // Create parent directory if needed
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
    }

    // Atomic write: write to temp file then rename
    std::string tmpPath = path + ".tmp";
    {
        std::ofstream f(tmpPath);
        if (!f) {
            throw std::runtime_error("Cannot write runner state to '" + tmpPath + "'");
        }
        f << j.dump(2) << "\n";
    }

    // Atomic rename (POSIX guarantee)
    if (std::rename(tmpPath.c_str(), path.c_str()) != 0) {
        throw std::runtime_error("Failed to rename '" + tmpPath + "' to '" + path + "'");
    }
}

// ─── defaultRunnerStatePath ───────────────────────────────────────────────

std::string defaultRunnerStatePath() {
#ifdef __HAIKU__
    BPath settingsPath;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &settingsPath) == B_OK) {
        settingsPath.Append("act_runner");
        settingsPath.Append(".runner");
        return settingsPath.Path();
    }
    return ".runner";
#else
    return ".runner";
#endif
}

} // namespace runner
