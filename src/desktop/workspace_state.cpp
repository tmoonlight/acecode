#include "workspace_state.hpp"

#include "utils/state_file.hpp"

#include <nlohmann/json.hpp>

namespace acecode {

std::string read_last_active_workspace_hash() {
    auto j = read_state_json();
    if (!j.contains("last_active_workspace_hash")) return "";
    if (!j["last_active_workspace_hash"].is_string()) return "";
    return j["last_active_workspace_hash"].get<std::string>();
}

void write_last_active_workspace_hash(const std::string& hash) {
    (void)update_state_json([hash](nlohmann::json& state) {
        state["last_active_workspace_hash"] = hash;
        return true;
    });
}

std::string read_last_home_workspace_hash() {
    auto j = read_state_json();
    if (!j.contains("last_home_workspace_hash")) return "";
    if (!j["last_home_workspace_hash"].is_string()) return "";
    return j["last_home_workspace_hash"].get<std::string>();
}

void write_last_home_workspace_hash(const std::string& hash) {
    (void)update_state_json([hash](nlohmann::json& state) {
        state["last_home_workspace_hash"] = hash;
        return true;
    });
}

} // namespace acecode
