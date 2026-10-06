#include "im/line/line_protocol.hpp"

#include "im/markdown.hpp"
#include "im/text_chunk.hpp"
#include "platform/crypto/digest.hpp"
#include "utils/base64.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace acecode::im::line {
namespace {

std::string str(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return {};
    const auto it = object.find(key);
    if (it == object.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<std::int64_t>());
    if (it->is_number_unsigned()) return std::to_string(it->get<std::uint64_t>());
    return {};
}

std::int64_t int64_of(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return 0;
    const auto it = object.find(key);
    if (it == object.end()) return 0;
    if (it->is_number_integer()) return it->get<std::int64_t>();
    if (it->is_number_unsigned()) return static_cast<std::int64_t>(it->get<std::uint64_t>());
    if (it->is_number_float()) return static_cast<std::int64_t>(it->get<double>());
    return 0;
}

bool bool_of(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return false;
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// UTF-8 序列长度与码点;非法字节按 1 字节、1 个 UTF-16 单位处理。
std::size_t sequence_length(const std::string& utf8, std::size_t i, std::uint32_t* code_point) {
    const auto c = static_cast<unsigned char>(utf8[i]);
    std::size_t length = 1;
    std::uint32_t value = c;
    if (c >= 0xF0 && c <= 0xF4) {
        length = 4;
        value = c & 0x07u;
    } else if (c >= 0xE0) {
        length = 3;
        value = c & 0x0Fu;
    } else if (c >= 0xC2 && c < 0xE0) {
        length = 2;
        value = c & 0x1Fu;
    }
    if (length == 1 || i + length > utf8.size()) {
        *code_point = c;
        return 1;
    }
    for (std::size_t k = 1; k < length; ++k) {
        const auto next = static_cast<unsigned char>(utf8[i + k]);
        if ((next & 0xC0u) != 0x80u) {
            *code_point = c;
            return 1;
        }
        value = (value << 6) | (next & 0x3Fu);
    }
    *code_point = value;
    return length;
}

EventKind kind_of(const std::string& type) {
    if (type == "message") return EventKind::Message;
    if (type == "follow") return EventKind::Follow;
    if (type == "unfollow") return EventKind::Unfollow;
    if (type == "join") return EventKind::Join;
    if (type == "leave") return EventKind::Leave;
    if (type == "unsend") return EventKind::Unsend;
    if (type == "messageEdited") return EventKind::MessageEdited;
    if (type == "postback") return EventKind::Postback;
    return EventKind::Other;
}

Attachment content_attachment(const nlohmann::json& message, AttachmentKind kind, const std::string& extension,
                              const std::string& mime) {
    Attachment attachment;
    attachment.kind = kind;
    const auto id = str(message, "id");
    attachment.name = id.empty() ? "line" + extension : id + extension;
    attachment.mime_type = mime;
    const auto provider = message.value("contentProvider", nlohmann::json::object());
    if (str(provider, "type") == "external") {
        // 外部内容不能经 LINE 内容接口下载,只能取对方给出的地址。
        attachment.remote_ref = str(provider, "originalContentUrl");
    } else {
        attachment.remote_ref = id;
    }
    return attachment;
}

std::string location_text(const nlohmann::json& message) {
    std::string text = "[位置]";
    const auto title = str(message, "title");
    const auto address = str(message, "address");
    if (!title.empty()) text += " " + title;
    if (!address.empty()) text += " " + address;
    if (message.contains("latitude") && message.contains("longitude") && message["latitude"].is_number() &&
        message["longitude"].is_number()) {
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), " (%.6f, %.6f)", message["latitude"].get<double>(),
                      message["longitude"].get<double>());
        text += buffer;
    }
    return text;
}

void parse_message(WebhookEvent& event, const nlohmann::json& message, const std::string& account,
                   std::int64_t received_at_ms) {
    event.message_id = str(message, "id");
    event.quoted_message_id = str(message, "quotedMessageId");
    const auto type = str(message, "type");

    std::string text;
    std::vector<Attachment> attachments;
    if (type == "text") {
        text = str(message, "text");
        std::vector<std::pair<std::size_t, std::size_t>> strip;
        const auto mention = message.value("mention", nlohmann::json::object());
        const auto mentionees =
            mention.is_object() ? mention.value("mentionees", nlohmann::json::array()) : nlohmann::json::array();
        if (mentionees.is_array()) {
            for (const auto& m : mentionees) {
                if (!m.is_object() || str(m, "type") != "user") continue;  // @All 不算点名机器人
                const bool self = bool_of(m, "isSelf") || (!account.empty() && str(m, "userId") == account);
                if (!self) continue;
                event.mentioned_self = true;
                const auto index = int64_of(m, "index");
                const auto length = int64_of(m, "length");
                if (index >= 0 && length > 0)
                    strip.emplace_back(static_cast<std::size_t>(index), static_cast<std::size_t>(length));
            }
        }
        text = trim(remove_utf16_ranges(text, std::move(strip)));
    } else if (type == "image") {
        attachments.push_back(content_attachment(message, AttachmentKind::Image, ".jpg", "image/jpeg"));
    } else if (type == "video") {
        attachments.push_back(content_attachment(message, AttachmentKind::Video, ".mp4", "video/mp4"));
    } else if (type == "audio") {
        attachments.push_back(content_attachment(message, AttachmentKind::Voice, ".m4a", "audio/mp4"));
    } else if (type == "file") {
        Attachment attachment;
        attachment.kind = AttachmentKind::File;
        attachment.name = str(message, "fileName");
        if (attachment.name.empty()) attachment.name = "file";
        attachment.mime_type = "application/octet-stream";
        const auto size = int64_of(message, "fileSize");
        attachment.size = size > 0 ? static_cast<std::uint64_t>(size) : 0;
        attachment.remote_ref = event.message_id;
        attachments.push_back(std::move(attachment));
    } else if (type == "sticker") {
        Attachment attachment;
        attachment.kind = AttachmentKind::Sticker;
        attachment.name = "sticker";
        attachments.push_back(std::move(attachment));
    } else if (type == "location") {
        text = location_text(message);
    } else {
        return;  // 未知类型:平台会无通知新增类型,一律忽略
    }

    if (event.chat_id.empty() || event.message_id.empty()) return;
    if (event.source_type != "user" && event.user_id.empty()) {
        event.unidentified_sender = true;
        return;
    }
    if (text.empty() && attachments.empty()) return;

    Inbound inbound;
    inbound.address.platform = kPlatform;
    inbound.address.account = account;
    inbound.address.kind = event.source_type == "user" ? ChatKind::Private : ChatKind::Group;
    inbound.address.chat = event.chat_id;
    inbound.address.sender = event.user_id;
    inbound.message_id = event.message_id;
    inbound.text = std::move(text);
    inbound.mentioned = inbound.address.kind == ChatKind::Private || event.mentioned_self;
    if (inbound.address.kind == ChatKind::Private && inbound.text.rfind("/start ", 0) == 0) {
        const auto code = trim(inbound.text.substr(7));
        if (!code.empty() && code.find_first_of(" \t\r\n") == std::string::npos) inbound.start_code = code;
    }
    inbound.attachments = std::move(attachments);
    if (!event.reply_token.empty() && !event.standby) {
        ReplyToken token;
        token.token = event.reply_token;
        token.received_at_ms = received_at_ms;
        token.event_ts_ms = event.timestamp_ms;
        token.redelivery = event.redelivery;
        token.quote_token = str(message, "quoteToken");
        inbound.reply_context = make_reply_context(token, event.message_id);
    } else {
        inbound.reply_context = nlohmann::json{{"message_id", event.message_id}};
        const auto quote = str(message, "quoteToken");
        if (!quote.empty()) inbound.reply_context["quote_token"] = quote;
    }
    event.inbound = std::move(inbound);
}

WebhookEvent parse_event(const nlohmann::json& e, const std::string& account, std::int64_t received_at_ms) {
    WebhookEvent event;
    event.type = str(e, "type");
    event.kind = kind_of(event.type);
    event.event_id = str(e, "webhookEventId");
    event.standby = str(e, "mode") == "standby";
    event.redelivery = bool_of(e.value("deliveryContext", nlohmann::json::object()), "isRedelivery");
    event.timestamp_ms = int64_of(e, "timestamp");
    event.reply_token = str(e, "replyToken");
    const auto source = e.value("source", nlohmann::json::object());
    event.source_type = str(source, "type");
    event.user_id = str(source, "userId");
    if (event.source_type == "user") event.chat_id = event.user_id;
    else if (event.source_type == "group") event.chat_id = str(source, "groupId");
    else if (event.source_type == "room") event.chat_id = str(source, "roomId");
    if (event.kind == EventKind::Message) {
        const auto message = e.value("message", nlohmann::json::object());
        if (message.is_object()) parse_message(event, message, account, received_at_ms);
    } else if (event.kind == EventKind::Unsend) {
        event.message_id = str(e.value("unsend", nlohmann::json::object()), "messageId");
    }
    return event;
}

} // namespace

std::string json_string(const nlohmann::json& object, const char* key) { return str(object, key); }

bool json_bool(const nlohmann::json& object, const char* key) { return bool_of(object, key); }

std::string sign_body(std::string_view raw_body, const std::string& channel_secret) {
    if (channel_secret.empty()) return {};
    const auto digest = platform::hmac_sha256(channel_secret, std::string(raw_body));
    if (digest.empty()) return {};
    return base64_encode(digest);
}

bool verify_signature(std::string_view raw_body, std::string_view signature, const std::string& channel_secret) {
    while (!signature.empty() && (signature.front() == ' ' || signature.front() == '\t')) signature.remove_prefix(1);
    while (!signature.empty() && (signature.back() == ' ' || signature.back() == '\t' || signature.back() == '\r' ||
                                  signature.back() == '\n'))
        signature.remove_suffix(1);
    if (signature.empty()) return false;
    const auto expected = sign_body(raw_body, channel_secret);
    if (expected.empty() || expected.size() != signature.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < expected.size(); ++i)
        diff |= static_cast<unsigned char>(expected[i] ^ signature[i]);
    return diff == 0;
}

ParsedWebhook parse_webhook(std::string_view body, const std::string& account, std::int64_t received_at_ms) {
    ParsedWebhook out;
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(body.begin(), body.end());
    } catch (...) {
        return out;
    }
    if (!json.is_object()) return out;
    out.valid = true;
    out.destination = str(json, "destination");
    const auto events = json.find("events");
    if (events == json.end() || !events->is_array()) return out;
    for (const auto& e : *events) {
        if (!e.is_object()) continue;
        out.events.push_back(parse_event(e, account, received_at_ms));
    }
    return out;
}

std::size_t utf16_to_byte_offset(const std::string& utf8, std::size_t units) {
    std::size_t i = 0;
    std::size_t counted = 0;
    while (i < utf8.size()) {
        if (counted >= units) return i;
        std::uint32_t code_point = 0;
        const auto length = sequence_length(utf8, i, &code_point);
        const std::size_t width = code_point >= 0x10000 ? 2 : 1;
        if (counted + width > units) return i;  // 落在代理对中间:取该字符开头
        counted += width;
        i += length;
    }
    return utf8.size();
}

std::string remove_utf16_ranges(const std::string& utf8, std::vector<std::pair<std::size_t, std::size_t>> ranges) {
    std::vector<std::pair<std::size_t, std::size_t>> bytes;
    for (const auto& [offset, length] : ranges) {
        if (length == 0) continue;
        const auto begin = utf16_to_byte_offset(utf8, offset);
        const auto end = utf16_to_byte_offset(utf8, offset + length);
        if (end > begin) bytes.emplace_back(begin, end);
    }
    if (bytes.empty()) return utf8;
    std::sort(bytes.begin(), bytes.end());
    std::string out;
    std::size_t cursor = 0;
    for (const auto& [begin, end] : bytes) {
        if (begin > cursor) out.append(utf8, cursor, begin - cursor);
        cursor = (std::max)(cursor, end);
    }
    if (cursor < utf8.size()) out.append(utf8, cursor, std::string::npos);
    return out;
}

std::optional<ReplyToken> reply_token_of(const nlohmann::json& reply_context) {
    if (!reply_context.is_object()) return std::nullopt;
    ReplyToken token;
    token.token = str(reply_context, "reply_token");
    if (token.token.empty()) return std::nullopt;
    token.received_at_ms = int64_of(reply_context, "received_at_ms");
    token.event_ts_ms = int64_of(reply_context, "event_ts_ms");
    token.redelivery = bool_of(reply_context, "redelivery");
    token.quote_token = str(reply_context, "quote_token");
    return token;
}

nlohmann::json make_reply_context(const ReplyToken& token, const std::string& message_id) {
    nlohmann::json context{{"reply_token", token.token},
                           {"received_at_ms", token.received_at_ms},
                           {"event_ts_ms", token.event_ts_ms}};
    if (token.redelivery) context["redelivery"] = true;
    if (!token.quote_token.empty()) context["quote_token"] = token.quote_token;
    if (!message_id.empty()) context["message_id"] = message_id;
    return context;
}

bool reply_token_fresh(const ReplyToken& token, std::int64_t now_ms) {
    if (token.token.empty() || token.received_at_ms <= 0) return false;
    if (now_ms >= token.received_at_ms + kReplyTokenWindowMs) return false;
    // 重投事件的令牌在事件发生 20 分钟后一律失效,再留 1 分钟余量。
    if (token.redelivery && (token.event_ts_ms <= 0 || now_ms >= token.event_ts_ms + kRedeliveredTokenLimitMs))
        return false;
    return true;
}

std::vector<std::string> plain_text_chunks(const std::string& markdown) {
    return chunk_text(markdown_to_plain(markdown), kChunkUnits, true);
}

nlohmann::json text_message(const std::string& text, const std::string& quote_token) {
    nlohmann::json message{{"type", "text"}, {"text", text}};
    if (!quote_token.empty()) message["quoteToken"] = quote_token;
    return message;
}

nlohmann::json image_message(const std::string& url) {
    return {{"type", "image"}, {"originalContentUrl", url}, {"previewImageUrl", url}};
}

std::vector<std::vector<nlohmann::json>> batch_messages(const std::vector<nlohmann::json>& messages,
                                                        std::size_t per_call) {
    if (per_call == 0) per_call = kMaxMessagesPerCall;
    std::vector<std::vector<nlohmann::json>> batches;
    for (std::size_t i = 0; i < messages.size(); i += per_call) {
        const auto end = (std::min)(messages.size(), i + per_call);
        batches.emplace_back(messages.begin() + static_cast<std::ptrdiff_t>(i),
                             messages.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return batches;
}

bool is_monthly_limit(long status, const std::string& message) {
    return status == 429 && lower(message).find("monthly limit") != std::string::npos;
}

bool is_invalid_reply_token(long status, const std::string& message) {
    return status == 400 && lower(message).find("reply token") != std::string::npos;
}

std::string percent_encode(std::string_view value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : value) {
        const auto u = static_cast<unsigned char>(c);
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-' || u == '_' ||
            u == '.' || u == '~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 0x0F]);
        }
    }
    return out;
}

std::string add_friend_url(const std::string& basic_id) {
    if (basic_id.empty()) return {};
    return "https://line.me/R/ti/p/" + percent_encode(basic_id);
}

std::string oa_message_url(const std::string& basic_id, const std::string& text) {
    if (basic_id.empty()) return {};
    return "https://line.me/R/oaMessage/" + percent_encode(basic_id) + "/?" + percent_encode(text);
}

bool is_user_id(const std::string& id) { return id.size() > 1 && id[0] == 'U'; }

bool is_line_image(const std::string& mime_type, const std::string& name) {
    const auto mime = lower(mime_type);
    if (mime == "image/jpeg" || mime == "image/jpg" || mime == "image/png") return true;
    if (!mime.empty() && mime != "application/octet-stream") return false;
    const auto lname = lower(name);
    return ends_with(lname, ".jpg") || ends_with(lname, ".jpeg") || ends_with(lname, ".png");
}

std::string safe_file_name(const std::string& name) {
    std::string clean;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        const bool ok = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || c == '.' ||
                        c == '-' || c == '_';
        clean.push_back(ok ? c : '_');
    }
    while (!clean.empty() && clean.front() == '.') clean.erase(clean.begin());
    std::string extension;
    const auto dot = clean.find_last_of('.');
    if (dot != std::string::npos && dot > 0 && clean.size() - dot <= 8) extension = clean.substr(dot);
    std::string stem = dot != std::string::npos && !extension.empty() ? clean.substr(0, dot) : clean;
    if (stem.find_first_not_of('_') == std::string::npos) stem = "file";
    const std::size_t limit = 64 - extension.size();
    if (stem.size() > limit) stem.resize(limit);
    return stem + extension;
}

} // namespace acecode::im::line
