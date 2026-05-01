#pragma once
// RunnerState.h — Persistent runner identity storage
//
// The runner state (token, UUID, name, labels) is saved as JSON in a file
// conventionally named ".runner" in the working directory or in the settings
// directory.  This mirrors the Go act_runner behaviour.

#include <string>
#include <vector>

namespace runner {

/// Persistent runner identity, saved to disk after successful registration.
struct RunnerState {
    std::string token;       ///< persistent runner token from RegisterResponse
    std::string uuid;        ///< server-assigned UUID
    std::string name;        ///< confirmed display name
    std::vector<std::string> labels; ///< "name:executor" strings

    bool empty() const { return token.empty() || uuid.empty(); }
};

// ─── Free functions ────────────────────────────────────────────────────────

/// Load runner state from a JSON file.
/// Returns an empty RunnerState (state.empty() == true) if file not found.
/// Throws std::runtime_error on JSON parse error.
RunnerState loadRunnerState(const std::string& path);

/// Persist runner state to a JSON file (atomic write via temp file + rename).
/// Throws std::runtime_error on failure.
void saveRunnerState(const RunnerState& state, const std::string& path);

/// Return the default runner state file path.
/// On Haiku: B_USER_SETTINGS_DIRECTORY/act_runner/.runner
/// Elsewhere: ./.runner
std::string defaultRunnerStatePath();

} // namespace runner
