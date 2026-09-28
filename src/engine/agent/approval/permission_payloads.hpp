#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace acecode { class SessionManager; struct CompactResult; }

namespace acecode::agent::detail {

nlohmann::json parse_tool_args_for_permission_payload(const std::string& args_json);

std::string build_plan_permission_args(const std::string& tool_name,
                                       const std::string& args_json,
                                       SessionManager* session_manager);

} // namespace acecode::agent::detail
