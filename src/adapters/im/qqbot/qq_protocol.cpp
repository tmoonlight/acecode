#include "qq_protocol.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::im::qqbot {
namespace {

std::string trim(const std::string& text) {
    std::size_t b = 0, e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) --e;
    return text.substr(b, e - b);
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string string_field(const nlohmann::json& value, const char* key) {
    if (!value.is_object() || !value.contains(key)) return {};
    const auto& field = value.at(key);
    if (field.is_string()) return field.get<std::string>();
    if (field.is_number_integer()) return std::to_string(field.get<std::int64_t>());
    return {};
}

Attachment parse_attachment(const nlohmann::json& raw) {
    Attachment attachment;
    const auto content_type = lower(string_field(raw, "content_type"));
    attachment.name = string_field(raw, "filename");
    attachment.mime_type = content_type;
    attachment.remote_ref = normalize_attachment_url(string_field(raw, "url"));
    if (raw.contains("size") && raw["size"].is_number_unsigned()) attachment.size = raw["size"].get<std::uint64_t>();
    else if (raw.contains("size") && raw["size"].is_number_integer() && raw["size"].get<std::int64_t>() > 0)
        attachment.size = static_cast<std::uint64_t>(raw["size"].get<std::int64_t>());
    const auto name = lower(attachment.name);
    const bool voice = content_type == "voice" || content_type.rfind("audio/", 0) == 0 ||
                       ends_with(name, ".amr") || ends_with(name, ".silk") || ends_with(name, ".ogg") ||
                       ends_with(name, ".wav") || ends_with(name, ".mp3");
    if (voice) {
        attachment.kind = AttachmentKind::Voice;
        attachment.transcript = trim(string_field(raw, "asr_refer_text"));
    } else if (content_type.rfind("image/", 0) == 0) {
        attachment.kind = AttachmentKind::Image;
    } else if (content_type.rfind("video/", 0) == 0) {
        attachment.kind = AttachmentKind::Video;
    } else {
        attachment.kind = AttachmentKind::File;
    }
    return attachment;
}

} // namespace

CloseDecision classify_close(int code) {
    switch (code) {
        case 4004: return {CloseAction::RefreshToken, "访问令牌失效,正在刷新"};
        case 4006: return {CloseAction::Identify, "会话超时,正在重新登录"};
        case 4007: return {CloseAction::Identify, "消息序号无效,正在重新登录"};
        case 4008: return {CloseAction::RateLimited, "触发 QQ 网关频控,稍后重连"};
        case 4009: return {CloseAction::Identify, "会话已失效,正在重新登录"};
        case 4914: return {CloseAction::Fatal, "机器人已下线或仅限沙箱环境使用"};
        case 4915: return {CloseAction::Fatal, "机器人已被封禁"};
        default: break;
    }
    if (code >= 4900 && code <= 4913) return {CloseAction::Identify, "QQ 网关会话错误,正在重新登录"};
    if (code >= 4001 && code <= 4003) return {CloseAction::Identify, "网关拒绝了请求,正在重新登录"};
    return {CloseAction::Resume, "连接已断开,正在重连"};
}

std::optional<Inbound> parse_message_event(const std::string& type, const nlohmann::json& d,
                                           const std::string& app_id, std::int64_t now_ms) {
    if (!d.is_object()) return std::nullopt;
    const bool c2c = type == "C2C_MESSAGE_CREATE";
    const bool group_at = type == "GROUP_AT_MESSAGE_CREATE";
    const bool group_all = type == "GROUP_MESSAGE_CREATE";
    if (!c2c && !group_at && !group_all) return std::nullopt;

    Inbound inbound;
    inbound.message_id = string_field(d, "id");
    if (inbound.message_id.empty()) return std::nullopt;
    const auto author = d.contains("author") ? d.at("author") : nlohmann::json::object();
    inbound.address.platform = "qq";
    inbound.address.account = app_id;
    if (c2c) {
        inbound.address.kind = ChatKind::Private;
        inbound.address.chat = inbound.address.sender = string_field(author, "user_openid");
        inbound.mentioned = true;
    } else {
        inbound.address.kind = ChatKind::Group;
        inbound.address.chat = string_field(d, "group_openid");
        inbound.address.sender = string_field(author, "member_openid");
        inbound.mentioned = group_at;
    }
    if (!inbound.address.valid()) return std::nullopt;
    inbound.text = trim(string_field(d, "content"));
    if (d.contains("attachments") && d["attachments"].is_array()) {
        for (const auto& raw : d["attachments"]) {
            if (raw.is_object()) inbound.attachments.push_back(parse_attachment(raw));
        }
    }
    if (inbound.text.empty() && inbound.attachments.empty()) return std::nullopt;
    inbound.reply_context = {{"msg_id", inbound.message_id},
                             {"scope", c2c ? "c2c" : "group"},
                             {"received_at_ms", now_ms}};
    return inbound;
}

ApiError parse_api_error(long status, const std::string& body) {
    ApiError error;
    error.status = status;
    try {
        const auto json = nlohmann::json::parse(body);
        if (json.is_object()) {
            if (json.contains("code") && json["code"].is_number_integer()) error.code = json["code"].get<long>();
            else if (json.contains("err_code") && json["err_code"].is_number_integer())
                error.code = json["err_code"].get<long>();
            error.message = json.value("message", std::string{});
        }
    } catch (...) {
    }
    if (error.message.empty()) error.message = "HTTP " + std::to_string(status);
    return error;
}

int file_type_for(const std::string& mime_type, const std::string& name) {
    const auto mime = lower(mime_type);
    const auto file = lower(name);
    if (mime.rfind("image/", 0) == 0) return kFileTypeImage;
    for (const char* ext : {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp"}) {
        if (ends_with(file, ext)) return kFileTypeImage;
    }
    if (mime == "video/mp4" || ends_with(file, ".mp4")) return kFileTypeVideo;
    return kFileTypeFile;
}

std::string normalize_attachment_url(const std::string& url) {
    if (url.rfind("//", 0) == 0) return "https:" + url;
    return url;
}

} // namespace acecode::im::qqbot
