#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>

namespace acecode::desktop::capture_probe {

inline bool require_object(const nlohmann::json& result, std::string& error,
                           const char* message) {
    if (!error.empty()) return false;
    if (result.is_object()) return true;
    error = message;
    return false;
}

template <typename Client>
nlohmann::json checked_command(Client& client, const std::string& method,
                               const nlohmann::json& params,
                               std::chrono::milliseconds timeout, std::string& error) {
    auto result = client.command(method, params, timeout, nullptr, error);
    if (!require_object(result, error, "native command returned a non-object result")) return {};
    return result;
}

}
