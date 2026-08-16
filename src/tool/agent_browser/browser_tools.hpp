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

// Exposed for focused tests of the visible Agent input indicator contract.
std::string agent_browser_pointer_script(
    double x,
    double y,
    const std::string& action);

// Installs or removes the short-lived listener used to surface synthetic
// pointer events dispatched by browser_evaluate without changing its result.
std::string agent_browser_evaluate_pointer_observer_script(bool install);

// Applies the platform-specific interaction trust fields used in successful
// Browser action results. Exposed so platform behavior stays unit-testable.
nlohmann::json agent_browser_input_result_metadata(
    nlohmann::json value,
    const std::string& input_mode);

} // namespace acecode::agent_browser
