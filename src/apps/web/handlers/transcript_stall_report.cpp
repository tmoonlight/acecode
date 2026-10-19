#include "transcript_stall_report.hpp"

#include <cmath>
#include <cstdint>
#include <string>

namespace acecode::web {

namespace {

constexpr const char* kKinds[] = {"stall_probe", "seq_reset", "heartbeat_timeout"};

// 标识类字段(会话 id、回合 id、实例号、状态名)只允许这些字符,避免日志注入。
bool is_token(const std::string& value, std::size_t max_size) {
    if (value.size() > max_size) return false;
    for (const char ch : value) {
        const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == '_' || ch == ':' || ch == '.';
        if (!ok) return false;
    }
    return true;
}

} // namespace

std::optional<nlohmann::json> sanitize_transcript_stall_report(const nlohmann::json& body) {
    if (!body.is_object()) return std::nullopt;
    const auto kind_it = body.find("kind");
    if (kind_it == body.end() || !kind_it->is_string()) return std::nullopt;
    const auto kind = kind_it->get<std::string>();
    bool known_kind = false;
    for (const auto* candidate : kKinds) known_kind = known_kind || kind == candidate;
    if (!known_kind) return std::nullopt;

    nlohmann::json out{{"kind", kind}};
    struct StringField { const char* key; std::size_t max_size; };
    static constexpr StringField kStrings[] = {
        {"session_id", 128}, {"reason", 48}, {"action", 32}, {"trigger", 32},
        {"client_epoch", 64}, {"server_epoch", 64},
        {"client_active_turn_id", 128}, {"server_active_turn_id", 128},
        {"ws_state", 16}, {"visibility", 16},
    };
    for (const auto& field : kStrings) {
        const auto it = body.find(field.key);
        if (it == body.end() || it->is_null()) continue;
        if (!it->is_string()) return std::nullopt;
        const auto value = it->get<std::string>();
        if (!is_token(value, field.max_size)) return std::nullopt;
        out[field.key] = value;
    }
    static constexpr const char* kNumbers[] = {
        "ms_since_event", "client_last_seq", "server_event_seq",
        "ws_ms_since_message", "heartbeat_timeouts", "ms_since_forced_reconnect",
    };
    for (const auto* key : kNumbers) {
        const auto it = body.find(key);
        if (it == body.end() || it->is_null()) continue;
        if (!it->is_number()) return std::nullopt;
        const auto value = it->get<double>();
        if (!std::isfinite(value) || value < 0 || value > 1e15) return std::nullopt;
        out[key] = static_cast<std::int64_t>(value);
    }
    static constexpr const char* kBooleans[] = {
        "server_busy", "server_active", "client_running",
    };
    for (const auto* key : kBooleans) {
        const auto it = body.find(key);
        if (it == body.end() || it->is_null()) continue;
        if (!it->is_boolean()) return std::nullopt;
        out[key] = it->get<bool>();
    }
    return out;
}

} // namespace acecode::web
