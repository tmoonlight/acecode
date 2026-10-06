#include "im/feishu/feishu_protocol.hpp"

#include "im/feishu/feishu_text_util.hpp"

#include <algorithm>
#include <map>

namespace acecode::im::feishu {

using namespace detail;

namespace {

// ---------------------------------------------------------------- 文本清理

bool is_self_mention(const Mention& mention, const BotIdentity& bot) {
    if (mention.key == "@_all") return false;
    if (!bot.open_id.empty() && !mention.open_id.empty()) return mention.open_id == bot.open_id;
    if (!bot.name.empty() && !mention.name.empty()) return mention.name == bot.name;
    return bot.open_id.empty() && mention.mentioned_type == "bot";
}

enum class TokenKind { Text, Self, Other, All };

struct Token {
    TokenKind kind = TokenKind::Text;
    std::string text;
};

// 只由空白与句末标点组成(含全角)。空串返回 true。
bool is_tail_only(std::string_view text) {
    static const char* const kWide[] = {"\xE3\x80\x82", "\xEF\xBC\x8C", "\xEF\xBC\x81", "\xEF\xBC\x9F",
                                        "\xEF\xBC\x9B", "\xEF\xBC\x9A", "\xE3\x80\x81", "\xE2\x80\xA6"};
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (is_space(c) || c == '.' || c == ',' || c == '!' || c == '?' || c == ';' || c == ':') {
            ++i;
            continue;
        }
        bool matched = false;
        for (const char* wide : kWide) {
            if (text.compare(i, 3, wide) == 0) {
                i += 3;
                matched = true;
                break;
            }
        }
        if (!matched) return false;
    }
    return true;
}

void ltrim_in_place(std::string& text) {
    std::size_t i = 0;
    while (i < text.size() && is_space(text[i])) ++i;
    text.erase(0, i);
}

void rtrim_in_place(std::string& text) {
    while (!text.empty() && is_space(text.back())) text.pop_back();
}

// 开头去掉机器人 @ 之后,顺手去掉紧跟的逗号 / 冒号("@Bot, 帮我…")。
void drop_leading_separator(std::string& text) {
    ltrim_in_place(text);
    for (const char* sep : {",", ":", "\xEF\xBC\x8C", "\xEF\xBC\x9A"}) {
        if (starts_with(text, sep)) {
            text.erase(0, std::char_traits<char>::length(sep));
            ltrim_in_place(text);
            return;
        }
    }
}

std::string normalize_lines(const std::string& text) {
    std::string unified;
    unified.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            unified.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            unified.push_back(text[i]);
        }
    }
    std::string out;
    out.reserve(unified.size());
    std::size_t start = 0;
    while (start <= unified.size()) {
        const auto end = unified.find('\n', start);
        std::string line = unified.substr(start, end == std::string::npos ? std::string::npos : end - start);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        out += line;
        if (end == std::string::npos) break;
        out.push_back('\n');
        start = end + 1;
    }
    return trim(out);
}

// ---------------------------------------------------------------- 内容提取

const nlohmann::json* resolve_post(const nlohmann::json& content, int depth = 0) {
    if (depth > 4 || !content.is_object()) return nullptr;
    if (content.contains("content") && content["content"].is_array()) return &content;
    if (content.contains("post")) {
        if (const auto* inner = resolve_post(content["post"], depth + 1)) return inner;
    }
    for (const char* locale : {"zh_cn", "en_us", "ja_jp"}) {
        if (content.contains(locale)) {
            if (const auto* inner = resolve_post(content[locale], depth + 1)) return inner;
        }
    }
    for (const auto& item : content.items()) {
        if (item.value().is_object() && item.value().contains("content") && item.value()["content"].is_array())
            return &item.value();
    }
    return nullptr;
}

Attachment resource_attachment(AttachmentKind kind, const std::string& message_id, const std::string& key,
                               const std::string& type, const std::string& name, const std::string& mime) {
    Attachment attachment;
    attachment.kind = kind;
    attachment.name = name;
    attachment.mime_type = mime;
    attachment.remote_ref = make_resource_ref(message_id, key, type);
    return attachment;
}

void render_post(const nlohmann::json& content, const std::string& message_id, ExtractedContent& out) {
    const auto* post = resolve_post(content);
    if (!post) {
        out.text = "[富文本消息]";
        return;
    }
    const auto title = trim(string_field(*post, "title"));
    std::vector<std::string> lines;
    for (const auto& row : (*post)["content"]) {
        if (!row.is_array()) continue;
        std::string line;
        for (const auto& node : row) {
            if (!node.is_object()) continue;
            const auto tag = string_field(node, "tag");
            const auto text = string_field(node, "text");
            if (tag == "text") {
                line += text;
            } else if (tag == "a") {
                const auto href = string_field(node, "href");
                line += href.empty() ? text : "[" + (text.empty() ? href : text) + "](" + href + ")";
            } else if (tag == "at") {
                const auto user = string_field(node, "user_id");
                if (starts_with(user, "@_")) {
                    line += user;  // 占位符交给 clean_text 统一处理
                } else {
                    const auto name = string_field(node, "user_name");
                    line += "@" + (name.empty() ? user : name);
                }
            } else if (tag == "img") {
                const auto key = string_field(node, "image_key");
                if (!key.empty())
                    out.attachments.push_back(resource_attachment(AttachmentKind::Image, message_id, key, "image", {}, {}));
            } else if (tag == "media") {
                const auto key = string_field(node, "file_key");
                if (!key.empty())
                    out.attachments.push_back(resource_attachment(AttachmentKind::Video, message_id, key, "file",
                                                                  string_field(node, "file_name"), {}));
            } else if (tag == "emotion") {
                line += "[" + string_field(node, "emoji_type") + "]";
            } else if (tag == "hr") {
                line += "---";
            } else if (tag == "code_block") {
                line += "```" + lower(string_field(node, "language")) + "\n" + text +
                        (ends_with(text, "\n") ? "" : "\n") + "```";
            } else {
                line += text;  // md 及未知但带 text 的节点
            }
        }
        if (!trim(line).empty()) lines.push_back(std::move(line));
    }
    std::string body;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) body.push_back('\n');
        body += lines[i];
    }
    if (title.empty()) out.text = body;
    else out.text = body.empty() ? title : title + "\n" + body;
}

void collect_card_text(const nlohmann::json& value, std::vector<std::string>& out, int depth) {
    if (depth > 32) return;
    if (value.is_object()) {
        // nlohmann 的对象按键名排序遍历;标题要排在正文前面,单独先取。
        if (value.contains("title") && value["title"].is_string()) {
            const auto title = trim(value["title"].get<std::string>());
            if (!title.empty() && (out.empty() || out.back() != title)) out.push_back(title);
        }
        for (const auto& item : value.items()) {
            const auto& key = item.key();
            const auto& field = item.value();
            if (key == "title" && field.is_string()) continue;
            if ((key == "text" || key == "content") && field.is_string()) {
                const auto text = trim(field.get<std::string>());
                if (!text.empty() && (out.empty() || out.back() != text)) out.push_back(text);
            } else if (field.is_structured()) {
                collect_card_text(field, out, depth + 1);
            }
        }
    } else if (value.is_array()) {
        for (const auto& item : value) collect_card_text(item, out, depth + 1);
    }
}

} // namespace

std::vector<Mention> parse_mentions(const nlohmann::json& mentions) {
    std::vector<Mention> out;
    if (!mentions.is_array()) return out;
    for (const auto& raw : mentions) {
        if (!raw.is_object()) continue;
        Mention mention;
        mention.key = string_field(raw, "key");
        mention.name = string_field(raw, "name");
        mention.mentioned_type = string_field(raw, "mentioned_type");
        if (raw.contains("id") && raw["id"].is_object()) {
            mention.open_id = string_field(raw["id"], "open_id");
            mention.user_id = string_field(raw["id"], "user_id");
        } else if (raw.contains("id") && raw["id"].is_string()) {
            // GET message 返回的 mentions:id 是字符串,类型在 id_type 里。
            const auto type = string_field(raw, "id_type");
            if (type == "open_id") mention.open_id = raw["id"].get<std::string>();
            else if (type == "user_id") mention.user_id = raw["id"].get<std::string>();
        }
        out.push_back(std::move(mention));
    }
    return out;
}

bool mentions_bot(const std::vector<Mention>& mentions, const BotIdentity& bot) {
    return std::any_of(mentions.begin(), mentions.end(),
                       [&bot](const Mention& mention) { return is_self_mention(mention, bot); });
}

std::string clean_text(const std::string& raw, const std::vector<Mention>& mentions, const BotIdentity& bot) {
    std::map<std::string, const Mention*> by_key;
    for (const auto& mention : mentions) {
        if (!mention.key.empty()) by_key.emplace(mention.key, &mention);
    }
    const auto self_label = [&bot](const Mention& mention) {
        return "@" + (!bot.name.empty() ? bot.name : !mention.name.empty() ? mention.name : std::string("机器人"));
    };

    std::vector<Token> tokens;
    const auto add_text = [&tokens](std::string_view piece) {
        if (!tokens.empty() && tokens.back().kind == TokenKind::Text) tokens.back().text.append(piece);
        else tokens.push_back({TokenKind::Text, std::string(piece)});
    };
    std::size_t i = 0;
    while (i < raw.size()) {
        if (raw.compare(i, 7, "@_user_") == 0 && i + 7 < raw.size() && is_digit(raw[i + 7])) {
            std::size_t j = i + 7;
            while (j < raw.size() && is_digit(raw[j])) ++j;
            const auto found = by_key.find(raw.substr(i, j - i));
            if (found == by_key.end()) {
                add_text(" ");
            } else if (is_self_mention(*found->second, bot)) {
                tokens.push_back({TokenKind::Self, self_label(*found->second)});
            } else {
                const auto& m = *found->second;
                tokens.push_back({TokenKind::Other,
                                  "@" + (!m.name.empty() ? m.name : !m.open_id.empty() ? m.open_id : std::string("用户"))});
            }
            i = j;
            continue;
        }
        if (raw.compare(i, 5, "@_all") == 0 && (i + 5 == raw.size() || !word_char(raw[i + 5]))) {
            tokens.push_back({TokenKind::All, "@all"});
            i += 5;
            continue;
        }
        add_text(std::string_view(raw).substr(i, 1));
        ++i;
    }

    // 开头的 @机器人(可以连着好几个)整体去掉。
    bool stripped_lead = false;
    while (!tokens.empty()) {
        const auto& front = tokens.front();
        if (front.kind == TokenKind::Self) {
            stripped_lead = true;
        } else if (!(front.kind == TokenKind::Text && trim(front.text).empty())) {
            break;
        }
        tokens.erase(tokens.begin());
    }
    if (stripped_lead && !tokens.empty() && tokens.front().kind == TokenKind::Text)
        drop_leading_separator(tokens.front().text);

    // 结尾的 @机器人 只在其后只剩空白 / 句末标点时去掉;句中的保留("别再 @机器人 了")。
    while (true) {
        std::ptrdiff_t k = static_cast<std::ptrdiff_t>(tokens.size()) - 1;
        while (k >= 0 && tokens[static_cast<std::size_t>(k)].kind == TokenKind::Text &&
               is_tail_only(tokens[static_cast<std::size_t>(k)].text))
            --k;
        if (k < 0 || tokens[static_cast<std::size_t>(k)].kind != TokenKind::Self) break;
        tokens.erase(tokens.begin() + k);
        if (k > 0 && tokens[static_cast<std::size_t>(k - 1)].kind == TokenKind::Text)
            rtrim_in_place(tokens[static_cast<std::size_t>(k - 1)].text);
    }

    std::string joined;
    for (const auto& token : tokens) joined += token.text;
    return normalize_lines(joined);
}

ExtractedContent extract_content(const std::string& message_type, const nlohmann::json& content,
                                 const std::string& message_id) {
    ExtractedContent out;
    if (message_type == "text") {
        out.text = string_field(content, "text");
    } else if (message_type == "post") {
        render_post(content, message_id, out);
    } else if (message_type == "image") {
        const auto key = string_field(content, "image_key");
        if (!key.empty())
            out.attachments.push_back(resource_attachment(AttachmentKind::Image, message_id, key, "image", {}, {}));
    } else if (message_type == "file") {
        const auto key = string_field(content, "file_key");
        if (!key.empty())
            out.attachments.push_back(resource_attachment(AttachmentKind::File, message_id, key, "file",
                                                          string_field(content, "file_name"), {}));
    } else if (message_type == "audio") {
        const auto key = string_field(content, "file_key");
        if (!key.empty())
            out.attachments.push_back(
                resource_attachment(AttachmentKind::Voice, message_id, key, "file", "voice.opus", "audio/opus"));
    } else if (message_type == "media") {
        const auto key = string_field(content, "file_key");
        if (!key.empty())
            out.attachments.push_back(resource_attachment(AttachmentKind::Video, message_id, key, "file",
                                                          string_field(content, "file_name"), {}));
    } else if (message_type == "sticker") {
        out.text = "[表情]";
    } else if (message_type == "folder") {
        const auto name = string_field(content, "file_name");
        out.text = "[文件夹" + (name.empty() ? std::string{} : ":" + name) + "](机器人无法下载文件夹)";
    } else if (message_type == "interactive") {
        std::vector<std::string> parts;
        collect_card_text(content, parts, 0);
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) out.text.push_back('\n');
            out.text += parts[i];
        }
        if (out.text.empty()) out.text = "[卡片消息]";
    } else if (message_type == "share_chat") {
        out.text = "[分享的群聊]";
    } else if (message_type == "share_user") {
        out.text = "[分享的名片]";
    } else if (message_type == "merge_forward") {
        out.text = "[合并转发的聊天记录]";
    } else if (message_type == "location") {
        const auto name = string_field(content, "name");
        out.text = "[位置]" + (name.empty() ? std::string{} : " " + name);
    } else if (message_type == "hongbao") {
        out.text = "[红包]";
    } else {
        out.text = "[" + (message_type.empty() ? std::string("未知") : message_type) + " 消息]";
    }
    return out;
}

ParsedEvent parse_event(const std::string& payload, const BotIdentity& bot, const std::string& account,
                        std::int64_t now_epoch_ms) {
    ParsedEvent parsed;
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(payload);
    } catch (...) {
        parsed.disposition = EventDisposition::Invalid;
        return parsed;
    }
    if (!root.is_object()) {
        parsed.disposition = EventDisposition::Invalid;
        return parsed;
    }
    const auto& header = object_field(root, "header");
    parsed.event_type = string_field(header, "event_type");
    // 1.0 事件(没有 schema 2.0)只有在用户额外订阅了旧版事件时才会出现,一律忽略。
    if (string_field(root, "schema") != "2.0" || parsed.event_type != "im.message.receive_v1") return parsed;

    const auto& event = object_field(root, "event");
    const auto& sender = object_field(event, "sender");
    const auto& message = object_field(event, "message");
    parsed.message_id = string_field(message, "message_id");
    const auto open_id = string_field(object_field(sender, "sender_id"), "open_id");
    const auto sender_type = string_field(sender, "sender_type");
    if (parsed.message_id.empty() || open_id.empty()) {
        parsed.disposition = EventDisposition::Invalid;
        return parsed;
    }
    if ((!sender_type.empty() && sender_type != "user") || (!bot.open_id.empty() && open_id == bot.open_id)) {
        parsed.disposition = EventDisposition::FromBot;
        return parsed;
    }
    const auto created = int_field(message, "create_time", 0);
    if (created > 0 && now_epoch_ms - created > kStaleEventMs) {
        parsed.disposition = EventDisposition::Stale;
        return parsed;
    }

    const auto chat_type = string_field(message, "chat_type");
    const auto chat_id = string_field(message, "chat_id");
    Inbound inbound;
    inbound.message_id = parsed.message_id;
    inbound.address.platform = kPlatform;
    inbound.address.account = account;
    if (chat_type == "p2p") {
        inbound.address.kind = ChatKind::Private;
        inbound.address.chat = inbound.address.sender = open_id;
    } else {
        inbound.address.kind = ChatKind::Group;
        inbound.address.chat = chat_id;
        inbound.address.sender = open_id;
    }
    if (!inbound.address.valid()) {
        parsed.disposition = EventDisposition::Invalid;
        return parsed;
    }

    const auto mentions = parse_mentions(message.contains("mentions") ? message["mentions"] : nlohmann::json());
    nlohmann::json content = nlohmann::json::object();
    const auto raw_content = string_field(message, "content");
    if (!raw_content.empty()) {
        try {
            content = nlohmann::json::parse(raw_content);
        } catch (...) {
            content = nlohmann::json::object();
        }
    }
    auto extracted = extract_content(string_field(message, "message_type"), content, parsed.message_id);
    inbound.text = clean_text(extracted.text, mentions, bot);
    inbound.attachments = std::move(extracted.attachments);
    inbound.mentioned = inbound.address.kind == ChatKind::Private || mentions_bot(mentions, bot);
    if (inbound.text.empty() && inbound.attachments.empty()) {
        parsed.disposition = EventDisposition::Empty;
        return parsed;
    }
    inbound.reply_context = {{"message_id", parsed.message_id}, {"chat_id", chat_id}, {"chat_type", chat_type}};
    const auto thread_id = string_field(message, "thread_id");
    if (!thread_id.empty()) inbound.reply_context["thread_id"] = thread_id;
    parsed.inbound = std::move(inbound);
    parsed.disposition = EventDisposition::Message;
    return parsed;
}

} // namespace acecode::im::feishu
