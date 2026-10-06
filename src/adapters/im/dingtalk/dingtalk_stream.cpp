#include "dingtalk_stream.hpp"

#include "im/dingtalk/dingtalk_protocol.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::im::dingtalk {
namespace {

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

std::string header_value(const nlohmann::json& headers, const char* key) {
    if (!headers.is_object()) return {};
    const auto it = headers.find(key);
    if (it == headers.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<std::int64_t>());
    if (it->is_number_unsigned()) return std::to_string(it->get<std::uint64_t>());
    return {};
}

} // namespace

std::optional<StreamFrame> parse_stream_frame(const std::string& text) {
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(text);
    } catch (...) {
        return std::nullopt;
    }
    if (!json.is_object()) return std::nullopt;
    StreamFrame frame;
    if (json.contains("type") && json["type"].is_string()) frame.type = upper(json["type"].get<std::string>());
    if (frame.type.empty()) return std::nullopt;
    if (json.contains("headers") && json["headers"].is_object()) frame.headers = json["headers"];
    frame.topic = header_value(frame.headers, "topic");
    frame.message_id = header_value(frame.headers, "messageId");
    if (json.contains("data")) {
        const auto& data = json["data"];
        if (data.is_string()) frame.data = data.get<std::string>();
        else if (!data.is_null()) frame.data = data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    return frame;
}

std::string stream_reply(const std::string& message_id, int code, const std::string& message,
                         const std::string& data) {
    nlohmann::json reply{{"code", code},
                         {"headers", {{"contentType", "application/json"}, {"messageId", message_id}}},
                         {"message", message},
                         {"data", data}};
    return reply.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

StreamSession::StreamSession(std::chrono::milliseconds idle_timeout) : idle_timeout_(idle_timeout) {}

void StreamSession::on_connected(Clock::time_point now) {
    connected_at_ = now;
    last_frame_ = now;
    frames_ = 0;
}

StreamSession::Output StreamSession::on_frame(const std::string& text, Clock::time_point now,
                                              bool accept_messages) {
    Output out;
    // 任何下行文本帧都说明连接仍然活着,包括无法解析的帧。
    last_frame_ = now;
    ++frames_;
    const auto frame = parse_stream_frame(text);
    if (!frame) {
        out.invalid = true;
        return out;
    }
    if (frame->type == "SYSTEM") {
        out.system_topic = frame->topic.empty() ? std::string("(none)") : frame->topic;
        // ping 必须把 data(含 opaque)原样回显;其它 SYSTEM 帧回执无害(python SDK 一律回 200)。
        if (!frame->message_id.empty()) out.replies.push_back(stream_reply(frame->message_id, 200, "OK", frame->data));
        if (frame->topic == "disconnect") out.disconnect = true;
        return out;
    }
    if (frame->type == "CALLBACK") {
        if (frame->topic != kBotMessageTopic) {
            out.ignored_topic = frame->topic;
            if (!frame->message_id.empty())
                out.replies.push_back(stream_reply(frame->message_id, 404, "not implemented", ""));
            return out;
        }
        if (!accept_messages) {
            out.deferred = true;
            return out;
        }
        if (!frame->message_id.empty())
            out.replies.push_back(stream_reply(frame->message_id, 200, "OK", kCallbackAckData));
        out.messages.push_back({frame->message_id, frame->data});
        return out;
    }
    // EVENT 或未知类型:没有订阅,回 SUCCESS 让平台不再重投。
    out.ignored_topic = frame->type + " " + frame->topic;
    if (!frame->message_id.empty()) out.replies.push_back(stream_reply(frame->message_id, 200, "OK", kEventAckData));
    return out;
}

bool StreamSession::idle_expired(Clock::time_point now) const {
    if (idle_timeout_.count() <= 0) return false;
    return now - last_frame_ >= idle_timeout_;
}

std::chrono::milliseconds stream_backoff(std::size_t attempt, std::chrono::milliseconds base,
                                         std::chrono::milliseconds cap, std::chrono::milliseconds jitter) {
    if (base.count() <= 0) base = std::chrono::milliseconds(1);
    auto delay = base;
    for (std::size_t i = 0; i < attempt && delay < cap; ++i) delay *= 2;
    delay = std::min(delay, cap);
    if (jitter.count() > 0) delay += jitter;
    return std::min(delay, cap);
}

} // namespace acecode::im::dingtalk
