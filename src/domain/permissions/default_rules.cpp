#include "default_rules.hpp"
#include "permissions.hpp"

namespace acecode {

PermissionMode parse_tui_permission_mode_name(std::string mode) {
    return PermissionManager::parse_mode_name(std::move(mode))
        .value_or(PermissionMode::Default);
}

void configure_tui_default_permissions(PermissionManager& permissions,
                                  bool dangerous_mode,
                                  const std::string& default_permission_mode) {
    permissions.set_mode(parse_tui_permission_mode_name(default_permission_mode));
    if (dangerous_mode) {
        permissions.set_dangerous(true);
        permissions.set_mode(PermissionMode::Yolo);
    }

    // Register built-in safety rules (deny writes to sensitive files/dirs)
    permissions.add_rule({"file_write", "*.env", "", RuleAction::Deny, 100});
    permissions.add_rule({"file_edit", "*.env", "", RuleAction::Deny, 100});
    permissions.add_rule({"file_write", ".git/**", "", RuleAction::Deny, 100});
    permissions.add_rule({"file_edit", ".git/**", "", RuleAction::Deny, 100});
    permissions.add_rule({"apply_patch", "*.env", "", RuleAction::Deny, 100});
    permissions.add_rule({"apply_patch", ".git/**", "", RuleAction::Deny, 100});
    permissions.add_rule({"bash", "", "rm -rf /", RuleAction::Deny, 100});
}


} // namespace acecode
