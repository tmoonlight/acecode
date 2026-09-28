#include "permission_payloads.hpp"
#include "session/session_manager.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <utility>

namespace acecode::agent::detail {

nlohmann::json parse_tool_args_for_permission_payload(const std::string& args_json) {
    if (args_json.empty()) return nlohmann::json::object();
    try {
        auto parsed = nlohmann::json::parse(args_json);
        return parsed.is_object() ? parsed : nlohmann::json{{"raw", args_json}};
    } catch (...) {
        return nlohmann::json{{"raw", args_json}};
    }
}

std::string build_plan_permission_args(const std::string& tool_name,
                                       const std::string& args_json,
                                       SessionManager* session_manager) {
    nlohmann::json payload;
    payload["tool_args"] = parse_tool_args_for_permission_payload(args_json);
    if (tool_name == "EnterPlanMode") {
        payload["kind"] = "enter_plan_mode";
        if (session_manager) {
            payload["plan_file_path"] = session_manager->current_plan_file_path();
        }
        return payload.dump();
    }
    if (tool_name == "ExitPlanMode") {
        payload["kind"] = "plan_approval";
        if (session_manager) {
            payload["plan_file_path"] = session_manager->ensure_plan_file_path();
            payload["plan"] = session_manager->read_plan_file();
        }
        return payload.dump();
    }
    return args_json;
}

} // namespace acecode::agent::detail
