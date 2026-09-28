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
// { "sessions": [...], "total": N, "total_exact": bool, "has_more": bool }.
// Unlimited lists stay a raw array.
//
// total is an upper bound whenever total_exact is false: paging stops reading
// the project directory once the page is full, so the exact post-filter count
// is unknown. Clients deciding whether a workspace still has unloaded rows
// must read has_more, which is authoritative in that direction.
inline nlohmann::json bounded_session_list_body(
    nlohmann::json sessions,
    std::size_t total,
    int limit,
    bool total_exact = true,
    bool has_more = false) {
    if (limit <= 0) return sessions;
    if (!sessions.is_array()) sessions = nlohmann::json::array();
    return nlohmann::json{
        {"sessions", std::move(sessions)},
        {"total", total},
        {"total_exact", total_exact},
        {"has_more", has_more},
    };
}

} // namespace acecode::web
