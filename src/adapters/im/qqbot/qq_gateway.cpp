#include "qq_gateway.hpp"

#include <algorithm>

namespace acecode::im::qqbot {

void GatewaySession::on_connected(std::string token, Clock::time_point now) {
    token_ = std::move(token);
    hello_ = false;
    awaiting_ack_ = false;
    ready_ = false;
    next_beat_ = now + interval_;
}

std::string GatewaySession::heartbeat_frame() const {
    nlohmann::json frame{{"op", 1}};
    frame["d"] = seq_ ? nlohmann::json(*seq_) : nlohmann::json(nullptr);
    return frame.dump();
}

GatewaySession::Output GatewaySession::on_frame(const std::string& text, Clock::time_point now) {
    Output out;
    nlohmann::json payload;
    try {
        payload = nlohmann::json::parse(text);
    } catch (...) {
        return out;  // 无效帧直接忽略,连接本身由心跳兜底
    }
    if (!payload.is_object() || !payload.contains("op") || !payload["op"].is_number_integer()) return out;
    const int op = payload["op"].get<int>();
    const auto d = payload.contains("d") ? payload["d"] : nlohmann::json(nullptr);

    switch (op) {
        case 10: {  // Hello:按服务端间隔的 80% 发心跳,随后登录或恢复
            hello_ = true;
            long long ms = 30000;
            if (d.is_object() && d.contains("heartbeat_interval") && d["heartbeat_interval"].is_number())
                ms = d["heartbeat_interval"].get<long long>();
            interval_ = std::chrono::milliseconds(std::max<long long>(1000, ms * 8 / 10));
            next_beat_ = now + interval_;
            nlohmann::json frame;
            if (can_resume()) {
                frame = {{"op", 6},
                         {"d", {{"token", "QQBot " + token_}, {"session_id", session_id_}, {"seq", *seq_}}}};
            } else {
                frame = {{"op", 2},
                         {"d",
                          {{"token", "QQBot " + token_},
                           {"intents", kIntents},
                           {"shard", nlohmann::json::array({0, 1})},
                           {"properties", {{"$os", "acecode"}, {"$browser", "acecode"}, {"$device", "acecode"}}}}}};
            }
            out.frames.push_back(frame.dump());
            break;
        }
        case 0: {  // Dispatch
            if (payload.contains("s") && payload["s"].is_number_integer()) seq_ = payload["s"].get<std::int64_t>();
            const auto type = payload.value("t", std::string{});
            if (type == "READY") {
                if (d.is_object()) {
                    session_id_ = d.value("session_id", std::string{});
                    if (d.contains("user") && d["user"].is_object()) {
                        const auto& user = d["user"];
                        if (user.contains("id") && user["id"].is_string()) out.bot_id = user["id"].get<std::string>();
                        out.bot_name = user.value("username", std::string{});
                    }
                }
                ready_ = true;
                out.ready = true;
            } else if (type == "RESUMED") {
                ready_ = true;
                out.ready = true;
            } else if (!type.empty()) {
                out.dispatches.emplace_back(type, d);
            }
            break;
        }
        case 11:  // Heartbeat ACK
            awaiting_ack_ = false;
            break;
        case 1:  // 服务端要求立即心跳
            out.frames.push_back(heartbeat_frame());
            break;
        case 7:  // 服务端要求重连,会话可以恢复
            out.reconnect = true;
            break;
        case 9:  // 会话失效;d=true 时仍可恢复
            if (!(d.is_boolean() && d.get<bool>())) clear_session();
            out.reconnect = true;
            break;
        default:
            break;
    }
    return out;
}

GatewaySession::Tick GatewaySession::on_tick(Clock::time_point now) {
    Tick tick;
    if (!hello_ || now < next_beat_) return tick;
    if (awaiting_ack_) {
        tick.zombie = true;
        return tick;
    }
    tick.heartbeat = heartbeat_frame();
    awaiting_ack_ = true;
    next_beat_ = now + interval_;
    return tick;
}

CloseDecision GatewaySession::on_closed(int code) {
    hello_ = false;
    ready_ = false;
    awaiting_ack_ = false;
    const auto decision = classify_close(code);
    if (decision.action == CloseAction::Identify) clear_session();
    return decision;
}

void GatewaySession::clear_session() {
    session_id_.clear();
    seq_.reset();
}

} // namespace acecode::im::qqbot
