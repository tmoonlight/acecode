#include "loop_request.hpp"
#include "loop_schedule.hpp"

#include <algorithm>

namespace acecode::loop {

bool parse_loop_request(const nlohmann::json& body,
                        std::int64_t now_ms,
                        const std::vector<std::string>& model_names,
                        const std::function<std::optional<std::string>(const std::string&)>& workspace_cwd,
                        LoopDefinition& out,
                        ValidationError& error) {
    if (!loop_from_json(body, out, &error)) return false;
    if (!body["schedule"].contains("timezone_offset_minutes")) {
        out.schedule.timezone_offset_minutes = current_timezone_offset_minutes(now_ms);
    }
    if (!out.workspace_hash.empty()) {
        const auto cwd = workspace_cwd(out.workspace_hash);
        if (!cwd || *cwd != out.workspace_cwd) {
            error = {"INVALID_WORKSPACE", "workspace_cwd",
                     "workspace is not registered or its path changed"};
            return false;
        }
    }
    if (std::find(model_names.begin(), model_names.end(), out.model_name) == model_names.end()) {
        error = {"INVALID_MODEL", "model_name", "selected model is unavailable"};
        return false;
    }
    return true;
}

} // namespace acecode::loop
