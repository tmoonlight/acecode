#include "im/feishu/feishu_gateway.hpp"

#include <algorithm>
#include <cmath>

namespace acecode::im::feishu {
namespace {

// 十进制非负整数;非法返回 -1。
long long parse_count(const std::string& text) {
    if (text.empty() || text.size() > 9) return -1;
    long long value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return -1;
        value = value * 10 + (c - '0');
    }
    return value;
}

const char* kAckOk = R"({"code":200})";

} // namespace

void LinkSession::on_connected(std::int32_t service_id, Clock::time_point now) {
    service_ = service_id;
    connected_ = true;
    next_ping_ = now;
    last_receive_ = now;
    pending_.clear();
}

void LinkSession::on_disconnected() {
    connected_ = false;
    pending_.clear();
}

std::chrono::milliseconds LinkSession::ping_interval() const {
    return std::chrono::seconds(std::max(1, config_.ping_interval_s));
}

std::chrono::milliseconds LinkSession::read_timeout() const { return ping_interval() * 2 + limits_.read_grace; }

void LinkSession::sweep(Clock::time_point now) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.expires <= now) it = pending_.erase(it);
        else ++it;
    }
}

std::optional<std::string> LinkSession::reassemble(const std::string& id, std::size_t sum, std::size_t seq,
                                                   std::string payload, Clock::time_point now,
                                                   std::string* warning) {
    auto found = pending_.find(id);
    if (found == pending_.end()) {
        if (!pending_.empty() && pending_.size() >= limits_.max_pending) {
            // 丢掉最快过期的那组,给新消息腾位置。
            auto oldest = std::min_element(pending_.begin(), pending_.end(), [](const auto& a, const auto& b) {
                return a.second.expires < b.second.expires;
            });
            pending_.erase(oldest);
            if (warning) *warning = "dropped an incomplete split message to bound the cache";
        }
        Pending fresh;
        fresh.parts.resize(sum);
        found = pending_.emplace(id, std::move(fresh)).first;
    }
    auto& entry = found->second;
    if (entry.parts.size() != sum) {
        pending_.erase(found);
        if (warning) *warning = "split message part count changed";
        return std::nullopt;
    }
    if (entry.parts[seq]) entry.bytes -= entry.parts[seq]->size();
    entry.bytes += payload.size();
    if (entry.bytes > limits_.max_total_bytes) {
        pending_.erase(found);
        if (warning) *warning = "split message exceeds the size limit";
        return std::nullopt;
    }
    entry.parts[seq] = std::move(payload);
    entry.expires = now + limits_.part_ttl;
    for (const auto& part : entry.parts) {
        if (!part) return std::nullopt;
    }
    std::string combined;
    combined.reserve(entry.bytes);
    for (const auto& part : entry.parts) combined += *part;
    pending_.erase(found);
    return combined;
}

LinkSession::Output LinkSession::on_message(const std::string& bytes, Clock::time_point now) {
    Output out;
    last_receive_ = now;
    sweep(now);
    Frame frame;
    std::string error;
    if (!decode_frame(bytes, &frame, &error)) {
        out.warning = "undecodable frame: " + error;
        return out;
    }
    const auto type = frame.header("type");
    if (frame.method == kMethodControl) {
        if (type == "pong" && frame.payload && !frame.payload->empty()) {
            try {
                out.config_updated = apply_client_config(nlohmann::json::parse(*frame.payload), &config_);
            } catch (...) {
                out.warning = "pong carried an invalid ClientConfig";
            }
        }
        return out;  // 服务端 ping 不回应
    }
    if (frame.method != kMethodData || type != "event") return out;  // card 等:不回应

    const auto sum_text = frame.header("sum");
    const auto seq_text = frame.header("seq");
    const long long sum = sum_text.empty() ? 1 : parse_count(sum_text);
    const long long seq = seq_text.empty() ? 0 : parse_count(seq_text);
    if (sum < 1 || seq < 0 || seq >= sum || static_cast<std::size_t>(sum) > limits_.max_parts) {
        out.warning = "invalid split header sum=" + sum_text + " seq=" + seq_text;
        return out;
    }
    std::string payload = frame.payload ? *frame.payload : std::string{};
    if (sum > 1) {
        auto combined = reassemble(frame.header("message_id"), static_cast<std::size_t>(sum),
                                   static_cast<std::size_t>(seq), std::move(payload), now, &out.warning);
        if (!combined) return out;  // 还没凑齐:非最后一片不回应
        payload = std::move(*combined);
    }
    // 先 ACK 再处理:3 秒期限只覆盖 ACK,处理放到调用方。
    out.frames.push_back(encode_frame(make_ack_frame(frame, 0, kAckOk)));
    out.events.push_back(std::move(payload));
    return out;
}

LinkSession::Tick LinkSession::on_tick(Clock::time_point now) {
    Tick tick;
    if (!connected_) return tick;
    if (now - last_receive_ >= read_timeout()) {
        tick.dead = true;
        return tick;
    }
    if (now >= next_ping_) {
        tick.ping = encode_frame(make_ping_frame(service_));
        next_ping_ = now + ping_interval();
    }
    sweep(now);
    return tick;
}

std::chrono::milliseconds reconnect_delay(const ClientConfig& config, bool server_config, std::size_t failures,
                                          const std::vector<std::chrono::milliseconds>& backoff, double jitter) {
    const auto ours = backoff.empty() ? std::chrono::milliseconds(1000)
                                      : backoff[std::min(failures, backoff.size() - 1)];
    if (!server_config) return ours;
    std::chrono::milliseconds server{0};
    if (failures == 0) {
        const double fraction = std::clamp(jitter, 0.0, 1.0);
        server = std::chrono::milliseconds(
            static_cast<long long>(std::llround(fraction * static_cast<double>(config.reconnect_nonce_s) * 1000.0)));
    } else {
        server = std::chrono::seconds(config.reconnect_interval_s);
    }
    return std::max(ours, server);
}

bool reconnect_exhausted(const ClientConfig& config, bool server_config, std::size_t failures) {
    return server_config && config.reconnect_count >= 0 &&
           failures >= static_cast<std::size_t>(config.reconnect_count);
}

} // namespace acecode::im::feishu
