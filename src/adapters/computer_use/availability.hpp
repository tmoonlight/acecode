#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace acecode::computer_use {
// Runs the delivered helper's noninteractive permission probe. Does not acquire
// the desktop lease, enable tools, revoke observations, or display prompts.
nlohmann::json availability();
// Only called by the authenticated user-facing settings route.
nlohmann::json request_permission(const std::string& permission);
}
