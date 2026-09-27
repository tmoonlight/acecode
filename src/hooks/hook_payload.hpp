#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace acecode {

nlohmann::json build_startup_before_model_load_payload(const std::string& cwd);

} // namespace acecode
