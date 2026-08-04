#pragma once

#include "tool/tool_executor.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace acecode::agent_browser {

std::vector<std::string> agent_browser_tool_names();
void register_agent_browser_tools(ToolExecutor& tools);
std::size_t unregister_agent_browser_tools(ToolExecutor& tools);

// Exposed for focused tests of the semantic reference contract.
std::string agent_browser_snapshot_script();

// Applies the platform-specific interaction trust fields used in successful
// Browser action results. Exposed so platform behavior stays unit-testable.
nlohmann::json agent_browser_input_result_metadata(
    nlohmann::json value,
    const std::string& input_mode);

} // namespace acecode::agent_browser
