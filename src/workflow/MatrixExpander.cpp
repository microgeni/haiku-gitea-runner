// MatrixExpander.cpp — Matrix cross-product expansion
#include "MatrixExpander.h"

#include <algorithm>

namespace runner {

static bool matchesPattern(const std::map<std::string,std::string>& combo,
                            const std::map<std::string,std::string>& pattern)
{
    for (auto& [k, v] : pattern) {
        auto it = combo.find(k);
        if (it == combo.end() || it->second != v) return false;
    }
    return true;
}

std::vector<std::map<std::string,std::string>> expandMatrix(const Matrix& matrix) {
    if (matrix.empty()) return {};

    // Step 1: Cross-product of axes
    std::vector<std::map<std::string,std::string>> combinations;
    combinations.push_back({});  // start with one empty combination

    for (auto& [axis_name, values] : matrix.axes) {
        std::vector<std::map<std::string,std::string>> expanded;
        for (auto& combo : combinations) {
            for (auto& val : values) {
                auto new_combo = combo;
                new_combo[axis_name] = val;
                expanded.push_back(std::move(new_combo));
            }
        }
        combinations = std::move(expanded);
    }

    // Step 2: Apply 'include' entries
    for (auto& include_entry : matrix.include) {
        bool matched_existing = false;

        // Check if this include matches any existing combination.
        // For augmentation, we only match on keys that already exist as axes
        // in the combination (not the extra keys being added).
        for (auto& combo : combinations) {
            // Build the subset of include_entry keys that are axes in this combo
            bool all_axes_match = true;
            bool has_axis_key   = false;

            for (auto& [k, v] : include_entry) {
                if (combo.find(k) != combo.end()) {
                    // This key is an existing axis — it must match
                    has_axis_key = true;
                    if (combo.at(k) != v) { all_axes_match = false; break; }
                }
                // Extra keys (not in combo) are the ones being added — skip for matching
            }

            if (has_axis_key && all_axes_match) {
                // Augment: add extra keys from include_entry that aren't already there
                for (auto& [k, v] : include_entry) {
                    if (combo.find(k) == combo.end()) {
                        combo[k] = v;
                    }
                }
                matched_existing = true;
            }
        }

        // If no existing combination matched, add as a brand-new row
        if (!matched_existing) {
            combinations.push_back(include_entry);
        }
    }

    // Step 3: Apply 'exclude' entries
    combinations.erase(
        std::remove_if(combinations.begin(), combinations.end(),
            [&](const std::map<std::string,std::string>& combo) {
                for (auto& exclude_entry : matrix.exclude) {
                    if (matchesPattern(combo, exclude_entry)) return true;
                }
                return false;
            }),
        combinations.end()
    );

    return combinations;
}

} // namespace runner
