#pragma once
// MatrixExpander.h — Job matrix expansion
//
// Takes a Matrix definition and expands it into a list of concrete
// key→value combinations, applying include/exclude rules.

#include "WorkflowParser.h"
#include <vector>
#include <map>
#include <string>

namespace runner {

/// Expands a Matrix into a list of concrete variable combinations.
///
/// Rules (GitHub Actions spec):
/// 1. Cross-product of all axes.
/// 2. Entries in 'include' that match an existing combination add extra keys.
/// 3. Entries in 'include' that don't match any combination become new rows.
/// 4. Entries matching 'exclude' patterns are removed.
///
/// Returns an empty vector for an empty or null matrix.
std::vector<std::map<std::string,std::string>> expandMatrix(const Matrix& matrix);

} // namespace runner
