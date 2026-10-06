#include "discord_gateway.hpp"

#include <algorithm>

namespace acecode::im::discord {
namespace {

const char* os_name() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string string_field(const nlohmann::json& value, const char* key) {
    if (!value.is_object()) return {};
    const auto it = value.find(key);
    if (it == value.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_unsigned()) return std::to_string(it->get<std::uint64_t>());
    if (it->is_number_integer()) return std::to_string(it->get<std::int64_t>());
    return {};
}

} // namespace

CloseDecision classify_close(int code) {
    switch (code) {
        case 4003: return {CloseAction::Identify, "Discord 网关会话已失效,正在重新登录"};
        case 4004: return {CloseAction::Fatal, "Bot Token 无效或已被重置,请在 Discord 开发者后台重新复制并保存"};
        case 4007: return {CloseAction::Identify, "Discord 网关消息序号无效,正在重新登录"};
        case 4008: return {CloseAction::RateLimited, "触发 Discord 网关频控,稍后自动重连"};
        case 4009: return {CloseAction::Identify, "Discord 网关会话超时,正在重新登录"};
        case 4010: return {CloseAction::Fatal, "Discord 网关拒绝了分片参数(ACECode 内部错误)"};
        case 4011: return {CloseAction::Fatal, "机器人加入的服务器过多(超过 2500 个),需要分片连接,暂不支持"};
        case 4012: return {CloseAction::Fatal, "Discord 网关不接受当前 API 版本(ACECode 内部错误)"};
        case 4013: return {CloseAction::Fatal, "Discord 网关拒绝了 intents 参数(ACECode 内部错误)"};
        case 4014: return {CloseAction::Fatal, "需要在 Discord 开发者后台开启 Message Content Intent"};
        default: break;
    }
    // 4000/4001/4002/4005、1006(没有关闭帧)以及服务端发来的其它 1xxx:都按普通断线恢复;
    // 恢复不了时服务端会回 op 9,届时再重新登录。
    return {CloseAction::Resume, "与 Discord 的连接已断开,正在恢复"};
}

void GatewaySession::on_connected(std::string token, Clock::time_point now, double jitter) {
    token_ = std::move(token);
    connected_ = true;
    hello_ = false;
    awaiting_ack_ = false;
    ready_ = false;
    jitter_ = jitter < 0.0 ? 0.0 : (jitter >= 1.0 ? 0.999 : jitter);
    hello_deadline_ = now + hello_timeout_;
    next_beat_ = now + interval_;
}

std::string GatewaySession::heartbeat_frame() const {
    nlohmann::json frame{{"op", 1}};
    frame["d"] = seq_ ? nlohmann::json(*seq_) : nlohmann::json(nullptr);
    return frame.dump();
}

std::string GatewaySession::identify_frame() const {
    return nlohmann::json{{"op", 2},
                          {"d",
                           {{"token", token_},
                            {"intents", intents_},
                            {"properties", {{"os", os_name()}, {"browser", "acecode"}, {"device", "acecode"}}}}}}
        .dump();
}

std::string GatewaySession::resume_frame() const {
    nlohmann::json frame{{"op", 6}};
    frame["d"] = {{"token", token_}, {"session_id", session_id_}};
    frame["d"]["seq"] = seq_ ? nlohmann::json(*seq_) : nlohmann::json(nullptr);
    return frame.dump();
}

GatewaySession::Output GatewaySession::on_frame(const std::string& text, Clock::time_point now) {
    Output out;
    nlohmann::json payload;
    try {
        payload = nlohmann::json::parse(text);
    } catch (...) {
        return out;  // 无效帧忽略,连接本身由心跳兜底
    }
    if (!payload.is_object()) return out;
    const auto op_it = payload.find("op");
    if (op_it == payload.end() || !op_it->is_number_integer()) return out;
    const int op = op_it->get<int>();
    const auto d_it = payload.find("d");
    const nlohmann::json d = d_it == payload.end() ? nlohmann::json(nullptr) : *d_it;

    switch (op) {
        case 10: {  // Hello
            hello_ = true;
            long long ms = 41250;
            if (d.is_object() && d.contains("heartbeat_interval") && d["heartbeat_interval"].is_number())
                ms = d["heartbeat_interval"].get<long long>();
            interval_ = std::chrono::milliseconds((std::max)(1LL, ms));
            next_beat_ = now + std::chrono::milliseconds(static_cast<long long>(static_cast<double>(interval_.count()) * jitter_));
            awaiting_ack_ = false;
            if (can_resume()) {
                out.frames.push_back(resume_frame());
            } else {
                out.frames.push_back(identify_frame());
                out.identified = true;
            }
            break;
        }
        case 0: {  // Dispatch
            const auto s = payload.find("s");
            if (s != payload.end() && s->is_number_integer()) seq_ = s->get<std::int64_t>();
            const auto type = payload.contains("t") && payload["t"].is_string() ? payload["t"].get<std::string>()
                                                                                : std::string{};
            if (type == "READY") {
                ReadyInfo info;
                if (d.is_object()) {
                    session_id_ = string_field(d, "session_id");
                    resume_url_ = string_field(d, "resume_gateway_url");
                    if (d.contains("user") && d["user"].is_object()) {
                        info.bot_id = string_field(d["user"], "id");
                        info.bot_name = string_field(d["user"], "username");
                    }
                    if (d.contains("application") && d["application"].is_object()) {
                        const auto& app = d["application"];
                        info.application_id = string_field(app, "id");
                        if (app.contains("flags") && app["flags"].is_number_integer())
                            info.application_flags = app["flags"].get<std::int64_t>();
                    }
                    if (d.contains("guilds") && d["guilds"].is_array()) info.guild_count = d["guilds"].size();
                }
                ready_ = true;
                out.ready = true;
                out.ready_info = std::move(info);
            } else if (type == "RESUMED") {
                ready_ = true;
                out.ready = true;
                out.resumed = true;
            } else if (!type.empty()) {
                out.dispatches.emplace_back(type, d);
            }
            break;
        }
        case 11:  // Heartbeat ACK
            awaiting_ack_ = false;
            break;
        case 1:  // 服务端要求立即心跳;常规节奏不变
            out.frames.push_back(heartbeat_frame());
            break;
        case 7:  // 服务端要求重连,会话可以恢复(可能在 Hello 之前到达)
            out.reconnect = true;
            break;
        case 9:  // 会话失效;d=true 时仍可恢复
            if (!(d.is_boolean() && d.get<bool>())) {
                clear_session();
                out.invalid_session = true;
            }
            out.reconnect = true;
            break;
        default:
            break;
    }
    return out;
}

GatewaySession::Tick GatewaySession::on_tick(Clock::time_point now) {
    Tick tick;
    if (!connected_) return tick;
    if (!hello_) {
        tick.hello_timeout = now >= hello_deadline_;
        return tick;
    }
    if (now < next_beat_) return tick;
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
    connected_ = false;
    hello_ = false;
    ready_ = false;
    awaiting_ack_ = false;
    const auto decision = classify_close(code);
    if (decision.action == CloseAction::Identify || decision.action == CloseAction::Fatal) clear_session();
    return decision;
}

void GatewaySession::clear_session() {
    session_id_.clear();
    resume_url_.clear();
    seq_.reset();
}

IdentifyBudget::IdentifyBudget(Limits limits, std::int64_t window_start_ms, std::int64_t count)
    : limits_(limits), window_start_((std::max)(std::int64_t{0}, window_start_ms)),
      count_((std::max)(std::int64_t{0}, count)) {}

bool IdentifyBudget::window_active(std::int64_t now_ms) const {
    return window_start_ > 0 && now_ms >= window_start_ && now_ms - window_start_ < limits_.window.count();
}

bool IdentifyBudget::daily_cap_reached(std::int64_t now_ms) const {
    return window_active(now_ms) && count_ >= limits_.daily_cap;
}

std::int64_t IdentifyBudget::next_allowed_ms(std::int64_t now_ms) const {
    std::int64_t at = now_ms;
    if (last_identify_) {
        at = (std::max)(at, *last_identify_ + static_cast<std::int64_t>(limits_.min_interval.count()));
        if (failures_ > 0) {
            // 5s → 10s → 20s …;指数封顶在 2^20,避免溢出。
            const int shift = (std::min)(failures_ - 1, 20);
            const auto backoff = (std::min)(static_cast<std::int64_t>(limits_.min_interval.count()) << shift,
                                            static_cast<std::int64_t>(limits_.max_backoff.count()));
            at = (std::max)(at, *last_identify_ + backoff);
        }
    }
    at = (std::max)(at, blocked_until_);
    if (daily_cap_reached(now_ms)) at = (std::max)(at, window_start_ + static_cast<std::int64_t>(limits_.window.count()));
    return at;
}

void IdentifyBudget::record_identify(std::int64_t now_ms) {
    if (!window_active(now_ms)) {
        window_start_ = now_ms;
        count_ = 0;
    }
    ++count_;
    last_identify_ = now_ms;
}

void IdentifyBudget::record_ready() { failures_ = 0; }

void IdentifyBudget::record_failure() { ++failures_; }

void IdentifyBudget::block_until(std::int64_t until_ms) { blocked_until_ = (std::max)(blocked_until_, until_ms); }

} // namespace acecode::im::discord
