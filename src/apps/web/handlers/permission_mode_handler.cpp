#include "permission_mode_handler.hpp"

namespace acecode::web {

std::optional<PermissionMode> parse_permission_mode_name(const std::string& name) {
    // 别名(accept-edits / acceptEdits = auto)集中在 PermissionManager 维护。
    return PermissionManager::parse_mode_name(name);
}

nlohmann::json permission_mode_to_json(PermissionMode mode) {
    return nlohmann::json{
        {"mode", PermissionManager::mode_name(mode)},
        {"description", PermissionManager::mode_description(mode)},
    };
}

} // namespace acecode::web
