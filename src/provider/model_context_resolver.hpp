#pragma once

#include "../config/config.hpp"

#include <string>

namespace acecode {

int resolve_model_context_window(
    const AppConfig& config,
    const std::string& provider_name,
    const std::string& model,
    int fallback_context_window
);

} // namespace acecode