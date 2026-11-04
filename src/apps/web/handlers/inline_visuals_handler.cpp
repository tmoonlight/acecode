#include "inline_visuals_handler.hpp"

namespace acecode::web {

nlohmann::json inline_visuals_snapshot(const InlineVisualsConfig& cfg) {
    return nlohmann::json{{"enabled", cfg.enabled}};
}

bool parse_inline_visuals_request(
    const nlohmann::json& body,
    const InlineVisualsConfig& current,
    InlineVisualsConfig& out,
    std::string& error) {
    if (!body.is_object()) {
        error = "expected a JSON object";
        return false;
    }
    InlineVisualsConfig next = current;
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
