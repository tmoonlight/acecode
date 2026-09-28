#pragma once
#include "config/vocab/permission_mode.hpp"
#include <string>
namespace acecode {
class PermissionManager;
PermissionMode parse_tui_permission_mode_name(std::string mode);
// These seven additional rules apply to the TUI main session only.
void configure_tui_default_permissions(PermissionManager& permissions,
    bool dangerous_mode, const std::string& default_permission_mode);
}
