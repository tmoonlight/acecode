#include "dingtalk_protocol.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::im::dingtalk {
namespace {

constexpr int kMaxQuoteDepth = 3;

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// 除 ASCII 空白外,钉钉 @ 提及后常见的 U+2005(四分之一 em 空格)、不换行空格与全角空格也算空白。
std::size_t space_length(std::string_view text, std::size_t pos) {
    const auto c = static_cast<unsigned char>(text[pos]);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') return 1;
    if (starts_with(text.substr(pos), "\xC2\xA0")) return 2;
    if (starts_with(text.substr(pos), "\xE2\x80\x85")) return 3;
    if (starts_with(text.substr(pos), "\xE3\x80\x80")) return 3;
    return 0;
}

bool ends_with_space(std::string_view text, std::size_t end) {
    if (end == 0) return false;
    const auto c = static_cast<unsigned char>(text[end - 1]);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') return true;
    if (end >= 2 && text.substr(end - 2, 2) == "\xC2\xA0") return true;
    if (end >= 3 && (text.substr(end - 3, 3) == "\xE2\x80\x85" || text.substr(end - 3, 3) == "\xE3\x80\x80"))
        return true;
    return false;
}

std::string trim(std::string_view text) {
    std::size_t b = 0;
    while (b < text.size()) {
        const auto n = space_length(text, b);
        if (!n) break;
        b += n;
    }
    std::size_t e = text.size();
    while (e > b) {
        const auto c = static_cast<unsigned char>(text[e - 1]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            --e;
        } else if (e - b >= 2 && text.substr(e - 2, 2) == "\xC2\xA0") {
            e -= 2;
        } else if (e - b >= 3 &&
                   (text.substr(e - 3, 3) == "\xE2\x80\x85" || text.substr(e - 3, 3) == "\xE3\x80\x80")) {
            e -= 3;
        } else {
            break;
        }
    }
    return std::string(text.substr(b, e - b));
}

// 字段值按“字符串或数字”读取;其它类型(含缺失)返回空串。
std::string str(const nlohmann::json& value, const char* key) {
    if (!value.is_object()) return {};
    const auto it = value.find(key);
    if (it == value.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<std::int64_t>());
    if (it->is_number_unsigned()) return std::to_string(it->get<std::uint64_t>());
    return {};
}

std::int64_t int_field(const nlohmann::json& value, const char* key) {
    if (!value.is_object()) return 0;
    const auto it = value.find(key);
    if (it == value.end()) return 0;
    if (it->is_number_integer()) return it->get<std::int64_t>();
    if (it->is_number_unsigned()) return static_cast<std::int64_t>(it->get<std::uint64_t>());
    if (it->is_number_float()) return static_cast<std::int64_t>(it->get<double>());
    if (it->is_string()) {
        try {
            return std::stoll(it->get<std::string>());
        } catch (...) {
        }
    }
    return 0;
}

bool bool_field(const nlohmann::json& value, const char* key) {
    if (!value.is_object()) return false;
    const auto it = value.find(key);
    if (it == value.end()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) return lower(it->get<std::string>()) == "true";
    if (it->is_number_integer()) return it->get<std::int64_t>() != 0;
    return false;
}

// content 可能是对象,也可能是 JSON 编码的字符串;字符串解析失败时原样返回。
nlohmann::json unwrap(const nlohmann::json& value) {
    if (!value.is_string()) return value;
    const auto& text = value.get_ref<const std::string&>();
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || (text[first] != '{' && text[first] != '[')) return value;
    try {
        return nlohmann::json::parse(text);
    } catch (...) {
        return value;
    }
}

nlohmann::json child(const nlohmann::json& value, const char* key) {
    if (!value.is_object()) return nlohmann::json();
    const auto it = value.find(key);
    return it == value.end() ? nlohmann::json() : unwrap(*it);
}

// 地址里的 id:非空、不超过 128 字节、只有可打印 ASCII 且不含空白与引号。
bool id_ok(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    for (const char c : id) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7F || c == '"' || c == '\\') return false;
    }
    return true;
}

nlohmann::json rich_text_items(const nlohmann::json& data, const nlohmann::json& content) {
    for (const auto& candidate : {child(content, "richText"), child(content, "richTextList"),
                                  child(child(data, "richText"), "richTextList"), child(data, "richText")}) {
        if (candidate.is_array()) return candidate;
    }
    return nlohmann::json::array();
}

std::string join_rich_text(const nlohmann::json& items) {
    std::string out;
    for (const auto& item : items) {
        if (!item.is_object()) continue;
        const auto type = lower(str(item, "type"));
        if (!type.empty() && type != "text") continue;
        const auto text = trim(str(item, "text"));
        if (text.empty()) continue;
        if (!out.empty()) out += "\n";
        out += text;
    }
    return out;
}

Attachment make_attachment(AttachmentKind kind, const std::string& code, const std::string& name) {
    Attachment attachment;
    attachment.kind = kind;
    attachment.remote_ref = code;
    attachment.name = name;
    return attachment;
}

std::string download_code(const nlohmann::json& content) {
    auto code = str(content, "downloadCode");
    if (code.empty()) code = str(content, "pictureDownloadCode");
    return code;
}

// 富文本里的媒体项。语音项必须仍按语音处理(Hermes #38211)。
void collect_rich_media(const nlohmann::json& items, std::vector<Attachment>& out) {
    for (const auto& item : items) {
        if (!item.is_object()) continue;
        const auto code = download_code(item);
        if (code.empty()) continue;
        const auto type = lower(str(item, "type"));
        if (type == "voice" || type == "audio") {
            auto attachment = make_attachment(AttachmentKind::Voice, code, str(item, "fileName"));
            attachment.transcript = trim(str(item, "recognition"));
            out.push_back(std::move(attachment));
        } else if (type == "video") {
            out.push_back(make_attachment(AttachmentKind::Video, code, str(item, "fileName")));
        } else if (type == "file") {
            out.push_back(make_attachment(AttachmentKind::File, code, str(item, "fileName")));
        } else {
            out.push_back(make_attachment(AttachmentKind::Image, code, str(item, "fileName")));
        }
    }
}

std::string card_url(const nlohmann::json& content) {
    for (const char* key : {"biz_custom_action_url", "url", "docUrl"}) {
        const auto value = str(content, key);
        if (!value.empty()) return value;
    }
    const auto inner = child(content, "content");
    if (inner.is_string()) return trim(inner.get<std::string>());
    for (const char* key : {"url", "docUrl"}) {
        const auto value = str(inner, key);
        if (!value.empty()) return value;
    }
    return {};
}

std::string document_line(const nlohmann::json& content) {
    const auto title = trim(str(content, "title"));
    const auto url = trim(card_url(content));
    std::string line = "[文档]";
    if (!title.empty()) line += " " + title;
    if (!url.empty()) line += " " + url;
    return title.empty() && url.empty() ? std::string{} : line;
}

std::string quote_of(const nlohmann::json& holder, int depth) {
    if (!bool_field(holder, "isReplyMsg")) return {};
    const auto replied = child(holder, "repliedMsg");
    if (!replied.is_object()) return {};
    auto type = str(replied, "msgType");
    if (type.empty()) type = str(replied, "msgtype");
    return extract_text(type, child(replied, "content"), depth + 1);
}

bool mentioned_bot(const nlohmann::json& data) {
    const auto bot = str(data, "chatbotUserId");
    const bool has_flag = data.contains("isInAtList");
    const auto users = child(data, "atUsers");
    bool in_users = false;
    if (users.is_array() && !bot.empty()) {
        for (const auto& user : users) in_users = in_users || str(user, "dingtalkId") == bot;
    }
    if (bool_field(data, "isInAtList") || in_users) return true;
    // 两个字段都缺:企业内部机器人在群里本来只能收到 @ 它的消息。
    return !has_flag && !users.is_array();
}

// 去掉群消息里对机器人自己的 @。只有 atUsers 里没有别人、且正文恰好只有一个
// “位于开头或空白之后”的 @ 记号时才去掉,不碰邮箱、git@host 之类的文字。
std::string strip_bot_mention(const std::string& text, const nlohmann::json& data) {
    const auto bot = str(data, "chatbotUserId");
    const auto users = child(data, "atUsers");
    if (users.is_array()) {
        for (const auto& user : users) {
            const auto id = str(user, "dingtalkId");
            if (!id.empty() && id != bot) return text;  // 还 @ 了别人:分不清哪个是机器人
        }
    }
    std::vector<std::size_t> marks;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '@' && (i == 0 || ends_with_space(text, i))) marks.push_back(i);
    }
    if (marks.size() != 1) return text;
    const auto begin = marks.front();
    auto end = begin + 1;
    while (end < text.size() && !space_length(text, end)) ++end;
    if (end == begin + 1) return text;  // 孤立的 "@"
    // 只去掉位于正文开头或末尾的 @(选人插入的提及几乎都在这两处),中间的 @ 文字原样保留。
    const bool at_start = trim(std::string_view(text).substr(0, begin)).empty();
    const bool at_end = trim(std::string_view(text).substr(end)).empty();
    if (!at_start && !at_end) return text;
    if (end < text.size()) end += space_length(text, end);
    return text.substr(0, begin) + text.substr(end);
}

std::size_t utf8_length(std::string_view text, std::size_t pos) {
    const auto lead = static_cast<unsigned char>(text[pos]);
    std::size_t length = 1;
    if (lead >= 0xF0 && lead <= 0xF4) length = 4;
    else if (lead >= 0xE0) length = 3;
    else if (lead >= 0xC2 && lead <= 0xDF) length = 2;
    return pos + length > text.size() ? 1 : length;
}

bool numbered_line(std::string_view line) {
    const auto stripped = trim(line);
    std::size_t i = 0;
    while (i < stripped.size() && std::isdigit(static_cast<unsigned char>(stripped[i]))) ++i;
    return i > 0 && i + 1 < stripped.size() && stripped[i] == '.' &&
           (stripped[i + 1] == ' ' || stripped[i + 1] == '\t');
}

std::size_t leading_spaces(std::string_view line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return i;
}

} // namespace

bool is_encrypted_sender_id(const std::string& id) { return starts_with(id, "$:"); }

std::string extract_text(const std::string& msgtype, const nlohmann::json& content, int depth) {
    if (depth > kMaxQuoteDepth) return {};
    const auto value = unwrap(content);
    if (value.is_string()) return trim(value.get<std::string>());
    if (!value.is_object()) return {};
    const auto type = lower(msgtype);
    for (const char* key : {"text", "content"}) {
        const auto field = child(value, key);
        if (field.is_string()) {
            const auto text = trim(field.get<std::string>());
            if (!text.empty()) return text;
        } else if (field.is_object() && depth < kMaxQuoteDepth) {
            const auto text = extract_text(type, field, depth + 1);
            if (!text.empty()) return text;
        }
    }
    const auto rich = rich_text_items(nlohmann::json::object(), value);
    if (!rich.empty()) {
        const auto text = join_rich_text(rich);
        if (!text.empty()) return text;
    }
    if (type == "picture" || type == "image") return "[图片]";
    if (type == "file") {
        const auto name = trim(str(value, "fileName"));
        return name.empty() ? "[文件]" : "[文件] " + name;
    }
    if (type == "audio" || type == "voice") {
        const auto recognition = trim(str(value, "recognition"));
        return recognition.empty() ? "[语音]" : recognition;
    }
    if (type == "video") return "[视频]";
    if (type == "interactivecard" || type == "card") return document_line(value);
    return trim(str(value, "title"));
}

std::optional<Inbound> parse_robot_message(const nlohmann::json& data, const std::string& client_id,
                                           const std::string& fallback_id) {
    if (!data.is_object()) return std::nullopt;
    Inbound inbound;
    inbound.message_id = str(data, "msgId");
    if (inbound.message_id.empty()) inbound.message_id = fallback_id;
    if (inbound.message_id.empty()) return std::nullopt;

    const bool group = str(data, "conversationType") == "2";
    const auto staff = str(data, "senderStaffId");
    const auto user = staff.empty() ? str(data, "senderId") : staff;
    auto& address = inbound.address;
    address.platform = kPlatform;
    address.account = client_id;
    address.kind = group ? ChatKind::Group : ChatKind::Private;
    address.chat = group ? str(data, "conversationId") : user;
    address.sender = user;
    if (!id_ok(address.account) || !id_ok(address.chat) || !id_ok(address.sender)) return std::nullopt;
    inbound.mentioned = group ? mentioned_bot(data) : true;
    inbound.sender_name = trim(str(data, "senderNick"));

    const auto msgtype = lower(str(data, "msgtype"));
    const auto content = child(data, "content");
    std::string text;
    if (msgtype == "richtext") {
        const auto items = rich_text_items(data, content);
        text = join_rich_text(items);
        collect_rich_media(items, inbound.attachments);
        inbound.quote_text = quote_of(content, 0);
    } else if (msgtype == "picture" || msgtype == "image") {
        const auto code = download_code(content);
        if (!code.empty())
            inbound.attachments.push_back(make_attachment(AttachmentKind::Image, code, str(content, "fileName")));
    } else if (msgtype == "file") {
        const auto code = download_code(content);
        if (!code.empty()) {
            auto attachment = make_attachment(AttachmentKind::File, code, str(content, "fileName"));
            const auto size = int_field(content, "fileSize");
            if (size > 0) attachment.size = static_cast<std::uint64_t>(size);
            inbound.attachments.push_back(std::move(attachment));
        }
    } else if (msgtype == "audio" || msgtype == "voice") {
        // 平台已经给了识别文字:不再附带音频重新转写(Hermes:重转写常把好结果覆盖成失败)。
        auto attachment = make_attachment(AttachmentKind::Voice, download_code(content), {});
        attachment.transcript = trim(str(content, "recognition"));
        if (!attachment.remote_ref.empty() || !attachment.transcript.empty())
            inbound.attachments.push_back(std::move(attachment));
    } else if (msgtype == "video") {
        const auto code = download_code(content);
        if (!code.empty()) {
            const auto ext = lower(str(content, "videoType"));
            inbound.attachments.push_back(
                make_attachment(AttachmentKind::Video, code, ext.empty() ? std::string{} : "video." + ext));
        }
    } else if (msgtype == "interactivecard" || msgtype == "card") {
        text = document_line(content);
    } else if (msgtype == "unknownmsgtype") {
        // 机器人收不到的消息类型(平台只给一句提示):交给上层回复“暂不支持”。
        inbound.attachments.push_back(make_attachment(AttachmentKind::Other, {}, {}));
    } else {
        // text / markdown / reply 以及未来的新类型:先看 text.content,再看 content。
        const auto text_obj = child(data, "text");
        text = trim(str(text_obj, "content"));
        inbound.quote_text = quote_of(text_obj, 0);
        if (text.empty()) text = extract_text(msgtype, content);
        if (inbound.quote_text.empty()) inbound.quote_text = quote_of(content, 0);
    }
    if (group && !text.empty()) text = strip_bot_mention(text, data);
    inbound.text = trim(text);
    if (inbound.text.empty() && inbound.attachments.empty()) return std::nullopt;

    nlohmann::json context{{"conversationType", group ? "2" : "1"},
                           {"conversationId", str(data, "conversationId")},
                           {"senderStaffId", staff},
                           {"msgId", inbound.message_id}};
    const auto webhook = str(data, "sessionWebhook");
    if (!webhook.empty()) {
        context["sessionWebhook"] = webhook;
        context["sessionWebhookExpiredTime"] = int_field(data, "sessionWebhookExpiredTime");
    }
    const auto robot = str(data, "robotCode");
    if (!robot.empty()) context["robotCode"] = robot;
    inbound.reply_context = std::move(context);
    return inbound;
}

ApiError parse_api_error(long status, const std::string& body) {
    ApiError error;
    error.status = status;
    try {
        const auto json = nlohmann::json::parse(body);
        if (json.is_object()) {
            error.code = str(json, "code");
            error.errcode = static_cast<long>(int_field(json, "errcode"));
            error.message = str(json, "message");
            if (error.message.empty()) error.message = str(json, "errmsg");
            for (const char* key : {"requestid", "requestId", "request_id"}) {
                if (error.request_id.empty()) error.request_id = str(json, key);
            }
        }
    } catch (...) {
    }
    if (error.message.empty()) {
        if (!error.code.empty()) error.message = error.code;
        else if (status > 0) error.message = "HTTP " + std::to_string(status);
    }
    return error;
}

bool body_has_errcode(const std::string& body, ApiError* error) {
    try {
        const auto json = nlohmann::json::parse(body);
        if (!json.is_object() || !json.contains("errcode")) return false;
        if (int_field(json, "errcode") == 0) return false;
        if (error) *error = parse_api_error(200, body);
        return true;
    } catch (...) {
        return false;
    }
}

bool is_token_error(const ApiError& error) {
    return error.code == "InvalidAuthentication" || error.status == 401 || error.errcode == 40014 ||
           error.errcode == 42001;
}

Throttle throttle_kind(const ApiError& error) {
    if (contains_ci(error.code, "QpsLimit")) return Throttle::Qps;
    if (error.status == 429 || error.errcode == 130101 || error.errcode == 90018 || error.errcode == 90006)
        return Throttle::Rate;
    if (contains_ci(error.code, "send.too.fast") || contains_ci(error.code, "SendTooFast") ||
        contains_ci(error.code, "Throttling"))
        return Throttle::Rate;
    if (contains_ci(error.message, "too fast") || error.message.find("频率过快") != std::string::npos ||
        error.message.find("发送过快") != std::string::npos)
        return Throttle::Rate;
    return Throttle::None;
}

bool is_webhook_gone(const ApiError& error) {
    if (error.errcode == 300001 || error.status == 404) return true;
    return error.errcode != 0 && contains_ci(error.message, "session");
}

std::string describe_api_error(const ApiError& error) {
    if (error.status == 0)
        return error.message.empty() ? std::string("无法连接钉钉开放平台") : "无法连接钉钉开放平台:" + error.message;
    const bool permission = contains_ci(error.code, "AccessTokenPermissionDenied") ||
                            (contains_ci(error.code, "Forbidden.AccessDenied") && !contains_ci(error.code, "QpsLimit"));
    if (permission)
        return "应用缺少「企业内机器人发送消息」权限(qyapi_robot_sendmsg),请在钉钉开发者后台开通并重新发布";
    if (contains_ci(error.code, "notAllow")) return "机器人未开通主动发送消息,请在钉钉开发者后台检查机器人配置";
    if (error.errcode == 60020) return "本机出口 IP 不在钉钉应用的服务器 IP 白名单中";
    if (contains_ci(error.code, "invalidClientIdOrSecret") || error.code == "authFailed" || error.errcode == 40096)
        return "Client ID 或 Client Secret 不正确";
    if (contains_ci(error.code, "robotCode") || error.message.find("机器人编码") != std::string::npos)
        return "机器人编码错误:请确认机器人已添加到应用并重新发布";
    if (throttle_kind(error) != Throttle::None) return "钉钉发送过于频繁,请稍后再试";
    std::string text = "钉钉返回错误";
    if (!error.message.empty()) text += ":" + error.message;
    else if (error.status > 0) text += ":HTTP " + std::to_string(error.status);
    return text;
}

bool webhook_url_allowed(const std::string& url, const std::vector<std::string>& extra_bases) {
    for (const char c : url) {
        if (static_cast<unsigned char>(c) <= 0x20 || c == '\\') return false;
    }
    const auto lowered = lower(url);
    if (starts_with(lowered, "https://api.dingtalk.com/") || starts_with(lowered, "https://oapi.dingtalk.com/"))
        return true;
    for (auto base : extra_bases) {
        while (!base.empty() && base.back() == '/') base.pop_back();
        if (base.empty()) continue;
        if (starts_with(lowered, lower(base) + "/")) return true;
    }
    return false;
}

std::string markdown_title(std::string_view markdown, const std::string& fallback) {
    std::size_t pos = 0;
    while (pos <= markdown.size()) {
        auto end = markdown.find('\n', pos);
        if (end == std::string_view::npos) end = markdown.size();
        auto line = std::string(markdown.substr(pos, end - pos));
        pos = end + 1;
        std::size_t b = 0;
        while (b < line.size() && std::string_view("#*>-+ \t`").find(line[b]) != std::string_view::npos) ++b;
        std::string cleaned;
        for (std::size_t i = b; i < line.size(); ++i) {
            if (line[i] != '*' && line[i] != '`') cleaned.push_back(line[i]);
        }
        cleaned = trim(cleaned);
        if (cleaned.empty()) {
            if (end == markdown.size()) break;
            continue;
        }
        std::size_t cut = 0, chars = 0;
        while (cut < cleaned.size() && chars < kTitleChars) {
            cut += utf8_length(cleaned, cut);
            ++chars;
        }
        return trim(cleaned.substr(0, cut));
    }
    return fallback;
}

std::string normalize_markdown(std::string_view markdown) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (true) {
        const auto end = markdown.find('\n', pos);
        if (end == std::string_view::npos) {
            lines.emplace_back(markdown.substr(pos));
            break;
        }
        lines.emplace_back(markdown.substr(pos, end - pos));
        pos = end + 1;
    }
    std::string out;
    bool in_fence = false;
    std::size_t fence_indent = 0;
    std::string previous;
    bool first = true;
    for (auto& line : lines) {
        const auto indent = leading_spaces(line);
        const bool fence = std::string_view(line).substr(indent).rfind("```", 0) == 0;
        if (fence && !in_fence) {
            in_fence = true;
            fence_indent = indent;
            line = line.substr(indent);
        } else if (fence && in_fence) {
            in_fence = false;
            line = line.substr(std::min(indent, fence_indent));
            fence_indent = 0;
        } else if (in_fence) {
            line = line.substr(std::min(indent, fence_indent));
        } else if (numbered_line(line) && !first && !trim(previous).empty() && !numbered_line(previous)) {
            out += "\n";
        }
        if (!first) out += "\n";
        out += line;
        previous = line;
        first = false;
    }
    return out;
}

std::string file_extension(const std::string& name) {
    const auto slash = name.find_last_of("/\\");
    const auto base = slash == std::string::npos ? name : name.substr(slash + 1);
    const auto dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= base.size()) return {};
    return lower(base.substr(dot + 1));
}

bool is_image_file(const std::string& mime_type, const std::string& name) {
    const auto mime = lower(mime_type);
    if (mime == "image/png" || mime == "image/jpeg" || mime == "image/jpg" || mime == "image/gif" ||
        mime == "image/bmp")
        return true;
    if (!mime.empty() && mime.rfind("image/", 0) != 0 && mime != "application/octet-stream") return false;
    const auto ext = file_extension(name);
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "bmp";
}

std::string upload_type_for(const std::string& mime_type, const std::string& name) {
    if (is_image_file(mime_type, name)) return "image";
    const auto mime = lower(mime_type);
    const auto ext = file_extension(name);
    if (mime == "audio/amr" || mime == "audio/mpeg" || mime == "audio/mp3" || mime == "audio/wav" ||
        ext == "amr" || ext == "mp3" || ext == "wav")
        return "voice";
    return "file";
}

std::string url_encode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : value) {
        const auto u = static_cast<unsigned char>(c);
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-' ||
            u == '_' || u == '.' || u == '~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 0x0F]);
        }
    }
    return out;
}

} // namespace acecode::im::dingtalk
