#pragma once

#include "config/config.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace acecode::web {

nlohmann::json summary_generation_settings(const AppConfig& config);
bool apply_summary_generation_settings(AppConfig& config,
                                       const nlohmann::json& patch,
                                       std::string& error);

} // namespace acecode::web
