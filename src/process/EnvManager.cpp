// EnvManager.cpp — File-based GitHub Actions env protocol implementation
#include "EnvManager.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstdlib>

namespace runner {

// ─── Constructor ──────────────────────────────────────────────────────────

EnvManager::EnvManager(std::filesystem::path workspace_dir)
    : workspace_dir_(std::move(workspace_dir))
{}

// ─── File paths ───────────────────────────────────────────────────────────

std::string EnvManager::githubOutputPath()  const { return (workspace_dir_ / "_github_output").string();  }
std::string EnvManager::githubEnvPath()     const { return (workspace_dir_ / "_github_env").string();     }
std::string EnvManager::githubPathPath()    const { return (workspace_dir_ / "_github_path").string();    }
std::string EnvManager::githubStatePath()   const { return (workspace_dir_ / "_github_state").string();   }
std::string EnvManager::githubSummaryPath() const { return (workspace_dir_ / "_github_summary").string(); }
std::string EnvManager::githubEventPath()   const { return event_payload_path_; }

// ─── setup / cleanup ──────────────────────────────────────────────────────

void EnvManager::setup(const std::string& event_payload_json,
                        const std::string& event_name) {
    std::filesystem::create_directories(workspace_dir_);
    event_name_ = event_name;

    // Touch all protocol files so steps can safely append to them
    for (auto& p : {githubOutputPath(), githubEnvPath(), githubPathPath(),
                    githubStatePath(), githubSummaryPath()}) {
        std::ofstream f(p, std::ios::app);  // append mode = create if not exist
    }

    // Write event payload to a file and record its path
    if (!event_payload_json.empty()) {
        event_payload_path_ = (workspace_dir_ / "_github_event.json").string();
        std::ofstream ef(event_payload_path_, std::ios::trunc);
        ef << event_payload_json;
    }
}

void EnvManager::cleanup() {
    // Remove all protocol temp files
    for (auto& p : {githubOutputPath(), githubEnvPath(), githubPathPath(),
                    githubStatePath(), githubSummaryPath()}) {
        std::filesystem::remove(p);
    }
    if (!event_payload_path_.empty()) {
        std::filesystem::remove(event_payload_path_);
    }
}

// ─── currentEnv ───────────────────────────────────────────────────────────

std::vector<std::pair<std::string,std::string>> EnvManager::currentEnv() const {
    std::vector<std::pair<std::string,std::string>> result;

    // Protocol file paths
    result.emplace_back("GITHUB_OUTPUT",       githubOutputPath());
    result.emplace_back("GITHUB_ENV",          githubEnvPath());
    result.emplace_back("GITHUB_PATH",         githubPathPath());
    result.emplace_back("GITHUB_STATE",        githubStatePath());
    result.emplace_back("GITHUB_STEP_SUMMARY", githubSummaryPath());

    // Event payload file (written once during setup)
    if (!event_payload_path_.empty()) {
        result.emplace_back("GITHUB_EVENT_PATH", event_payload_path_);
    }
    if (!event_name_.empty()) {
        result.emplace_back("GITHUB_EVENT_NAME", event_name_);
    }

    // Accumulated env overrides from $GITHUB_ENV
    for (auto& [k, v] : env_overrides_) {
        result.emplace_back(k, v);
    }

    // Prepend PATH entries
    if (!path_prepends_.empty()) {
        std::string current_path;
        const char* p = getenv("PATH");
        if (p) current_path = p;

        std::string new_path;
        for (auto& pp : path_prepends_) {
            if (!new_path.empty()) new_path += ':';
            new_path += pp;
        }
        if (!current_path.empty()) new_path += ':' + current_path;
        result.emplace_back("PATH", new_path);
    }

    return result;
}

// ─── parseKeyValueFile ────────────────────────────────────────────────────

// Parses both formats:
//   simple:    KEY=VALUE
//   multiline: KEY<<_DELIMITER_\nVALUE\n_DELIMITER_
std::map<std::string,std::string> EnvManager::parseKeyValueFile(const std::string& path) {
    std::map<std::string,std::string> result;

    std::ifstream f(path);
    if (!f) return result;

    std::string line;
    while (std::getline(f, line)) {
        // Trim trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        auto delim_pos = line.find("<<");
        if (delim_pos != std::string::npos) {
            // Multiline format: KEY<<DELIMITER
            std::string key       = line.substr(0, delim_pos);
            std::string delimiter = line.substr(delim_pos + 2);
            std::string value;
            bool first = true;
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line == delimiter) break;
                if (!first) value += '\n';
                value += line;
                first = false;
            }
            if (!key.empty()) result[key] = value;
        } else {
            // Simple format: KEY=VALUE
            auto eq_pos = line.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = line.substr(0, eq_pos);
                std::string val = line.substr(eq_pos + 1);
                if (!key.empty()) result[key] = val;
            }
        }
    }

    return result;
}

// ─── parseOutputs ─────────────────────────────────────────────────────────

std::map<std::string,std::string> EnvManager::parseOutputs() const {
    return parseKeyValueFile(githubOutputPath());
}

// ─── applyEnvChanges ──────────────────────────────────────────────────────

void EnvManager::applyEnvChanges() {
    auto changes = parseKeyValueFile(githubEnvPath());
    for (auto& [k, v] : changes) {
        env_overrides_[k] = v;
    }
}

// ─── applyPathChanges ─────────────────────────────────────────────────────

void EnvManager::applyPathChanges() {
    std::ifstream f(githubPathPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            path_prepends_.push_back(line);
        }
    }
}

// ─── parseState ───────────────────────────────────────────────────────────

std::map<std::string,std::string> EnvManager::parseState() const {
    return parseKeyValueFile(githubStatePath());
}

// ─── resetBetweenSteps ────────────────────────────────────────────────────

void EnvManager::resetBetweenSteps() {
    // Truncate output and summary files (env/path/state are cumulative)
    truncateFile(githubOutputPath());
    truncateFile(githubSummaryPath());
}

void EnvManager::truncateFile(const std::string& path) const {
    std::ofstream f(path, std::ios::trunc);
}

} // namespace runner
