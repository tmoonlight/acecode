#include "tool_preamble_handler.hpp"

namespace acecode::web {

nlohmann::json tool_preamble_snapshot(const ToolPreambleConfig& cfg) {
    return nlohmann::json{{"enabled", cfg.enabled}};
}

bool parse_tool_preamble_request(
    const nlohmann::json& body,
    const ToolPreambleConfig& current,
    ToolPreambleConfig& out,
    std::string& error) {
    if (!body.is_object()) {
        error = "expected a JSON object";
        return false;
    }
    ToolPreambleConfig next = current;
    if (body.contains("enabled")) {
        if (!body["enabled"].is_boolean()) {
            error = "enabled must be a boolean";
            return false;
        }
        next.enabled = body["enabled"].get<bool>();
    }
    out = next;
    return true;
}

} // namespace acecode::web
