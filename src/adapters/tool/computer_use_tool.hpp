#pragma once

#include "tool_executor.hpp"
#include "config/config.hpp"

namespace acecode {
// Keep the control identity ahead of potentially large accessibility output so
// the ordinary persisted-output preview remains sufficient for the next action.
std::string format_computer_use_output(const nlohmann::json& output);
std::vector<ToolImpl> create_computer_use_tools();
void refresh_computer_use_tools(ToolExecutor& tools, const AppConfig& config);
} // namespace acecode
