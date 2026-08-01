#pragma once

#include "tool/tool_executor.hpp"

#include <string>
#include <vector>

namespace acecode::agent_browser {

std::vector<std::string> agent_browser_tool_names();
void register_agent_browser_tools(ToolExecutor& tools);
std::size_t unregister_agent_browser_tools(ToolExecutor& tools);

// Exposed for focused tests of the semantic reference contract.
std::string agent_browser_snapshot_script();

} // namespace acecode::agent_browser
