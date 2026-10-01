#pragma once

#include "workspace/workspace_registry.hpp"
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace acecode {

// Only the explicitly supplied registered workspaces are inspected. Each
// workspace samples its five largest canonical transcripts. No content leaves
// this function; cancellation is checked between directory entries and records.
nlohmann::json diagnose_session_data(
    const std::string& projects_dir,
    const std::vector<desktop::WorkspaceMeta>& workspaces,
    const std::function<bool()>& should_cancel = {});

} // namespace acecode
