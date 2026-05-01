#pragma once
// EnvManager.h — Environment variable management for Actions jobs
//
// Implements the GitHub Actions file-based protocols:
//   $GITHUB_OUTPUT  — steps set outputs: echo "key=value" >> $GITHUB_OUTPUT
//   $GITHUB_ENV     — steps set env vars: echo "KEY=VALUE" >> $GITHUB_ENV
//   $GITHUB_PATH    — steps prepend to PATH: echo "/new/dir" >> $GITHUB_PATH
//   $GITHUB_STATE   — pre/post state: echo "key=val" >> $GITHUB_STATE
//   $GITHUB_STEP_SUMMARY — step markdown summaries
//   $GITHUB_EVENT_PATH   — path to JSON file containing the event payload
//
// Each file uses the multiline value format from newer GitHub Actions:
//   key<<delimiter
//   value lines
//   delimiter
// or the simple single-line format:
//   key=value

#include <string>
#include <map>
#include <vector>
#include <filesystem>

namespace runner {

/// Manages the file-based env-passing protocol for a single job run.
class EnvManager {
public:
    /// @param workspace_dir  the job's temp workspace directory
    explicit EnvManager(std::filesystem::path workspace_dir);

    /// Initialize temp files; call before spawning the first step.
    /// @param event_payload_json  raw JSON event payload (may be empty)
    /// @param event_name          e.g. "push", "pull_request"
    void setup(const std::string& event_payload_json = "",
               const std::string& event_name = "");

    /// Clean up temp files; call after all steps complete.
    void cleanup();

    // ── Accessors for step environment ────────────────────────────────────

    /// Returns the current set of environment variables to inject into steps.
    /// Call after each step to pick up changes made by previous steps.
    std::vector<std::pair<std::string,std::string>> currentEnv() const;

    /// Path to $GITHUB_OUTPUT file.
    std::string githubOutputPath() const;
    /// Path to $GITHUB_ENV file.
    std::string githubEnvPath()    const;
    /// Path to $GITHUB_PATH file.
    std::string githubPathPath()   const;
    /// Path to $GITHUB_STATE file.
    std::string githubStatePath()  const;
    /// Path to $GITHUB_STEP_SUMMARY file.
    std::string githubSummaryPath() const;
    /// Path to $GITHUB_EVENT_PATH file (written once during setup).
    std::string githubEventPath() const;

    // ── Post-step processing ───────────────────────────────────────────────

    /// Parse $GITHUB_OUTPUT written by a step.
    /// Returns map of output_name → value.
    std::map<std::string,std::string> parseOutputs() const;

    /// Parse $GITHUB_ENV and update internal env map.
    void applyEnvChanges();

    /// Parse $GITHUB_PATH and prepend entries to internal PATH.
    void applyPathChanges();

    /// Parse $GITHUB_STATE written by pre-step.
    std::map<std::string,std::string> parseState() const;

    /// Clear output/env/path/summary files between steps (but keep state).
    void resetBetweenSteps();

private:
    std::filesystem::path workspace_dir_;

    // Internal env overrides accumulated across steps
    std::map<std::string, std::string> env_overrides_;
    std::vector<std::string>           path_prepends_;

    // Event payload file (written once during setup)
    std::string event_name_;
    std::string event_payload_path_;  // empty if no payload

    static std::map<std::string,std::string> parseKeyValueFile(const std::string& path);
    void truncateFile(const std::string& path) const;
};

} // namespace runner
