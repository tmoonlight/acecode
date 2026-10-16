#pragma once

// Implementations behind the ios_simulator and ios_simulator_build tools.

#include "tool/tool_executor.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace acecode::ios_simulator {

ToolResult run_simulator_action(const std::string& action, const nlohmann::json& args, const ToolContext& ctx);
ToolResult run_simulator_build(const nlohmann::json& args, const ToolContext& ctx);

// Compact, model-readable rendering of a helper ui_describe result. With a
// non-empty query only matching elements are listed (flat, with centers).
std::string format_accessibility(const nlohmann::json& result, const std::string& query, std::size_t max_lines);

} // namespace acecode::ios_simulator
