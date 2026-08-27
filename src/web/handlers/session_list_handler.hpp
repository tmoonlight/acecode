#pragma once

// Sidebar session lists only need a compact page of metadata. Query parsing
// and response wrapping stay Crow-free so they can be unit-tested.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

namespace acecode::web {

// 0 = unlimited. Invalid or non-positive input also returns 0.
inline int parse_session_list_limit(const char* raw) {
    if (!raw || !*raw) return 0;
    try {
        const int parsed = std::stoi(std::string(raw));
        if (parsed <= 0) return 0;
        return std::min(parsed, 10000);
    } catch (...) {
        return 0;
    }
}

// When limit > 0, wrap a (possibly truncated) array as
// { "sessions": [...], "total": N }. Unlimited lists stay a raw array.
inline nlohmann::json bounded_session_list_body(
    nlohmann::json sessions,
    std::size_t total,
    int limit) {
    if (limit <= 0) return sessions;
    if (!sessions.is_array()) sessions = nlohmann::json::array();
    return nlohmann::json{
        {"sessions", std::move(sessions)},
        {"total", total},
    };
}

} // namespace acecode::web
