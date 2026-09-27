#pragma once

#include "config/config.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace acecode::web {

bool computer_use_supported();
nlohmann::json computer_use_settings(const AppConfig& config);
bool apply_computer_use_settings(AppConfig& config,
                                 const nlohmann::json& patch,
                                 std::string& error);

} // namespace acecode::web
