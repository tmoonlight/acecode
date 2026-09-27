#pragma once

#include "config/config.hpp"

#include <nlohmann/json.hpp>

namespace acecode::web {

struct ModelConnectionTestResult {
    int status = 200;
    nlohmann::json body;
};

// Operates on a private configuration snapshot. Never persists a model, creates
// a session, or returns credentials / upstream response bodies.
ModelConnectionTestResult test_model_connection(
    const nlohmann::json& body, AppConfig snapshot);

} // namespace acecode::web
