#include "tg_update.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace acecode::im::telegram {
namespace {

std::size_t sequence_length(const std::string& text, std::size_t pos) {
    const auto lead = static_cast<unsigned char>(text[pos]);
    std::size_t length = 1;
    if (lead >= 0xF0 && lead <= 0xF4) length = 4;
    else if (lead >= 0xE0) length = 3;
    else if (lead >= 0xC2 && lead <= 0xDF) length = 2;
    return pos + length > text.size() ? 1 : length;
}

// UTF-16 单位位置 → UTF-8 字节位置。
std::size_t byte_offset(const std::string& text, std::size_t units) {
    std::size_t pos = 0, used = 0;
    while (pos < text.size() && used < units) {
        const auto length = sequence_length(text, pos);
        used += length == 4 ? 2 : 1;
        pos += length;
    }
    return pos;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string trim(const std::string& text) {
    std::size_t b = 0, e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) --e;
    return text.substr(b, e - b);
}

std::string id_string(const nlohmann::json& value) {
    if (value.is_number_integer()) return std::to_string(value.get<std::int64_t>());
    if (value.is_string()) return value.get<std::string>();
    return {};
}

std::string display_name(const nlohmann::json& from) {
    const auto username = from.value("username", std::string{});
    if (!username.empty()) return "@" + username;
    auto name = from.value("first_name", std::string{});
    const auto last = from.value("last_name", std::string{});
    if (!last.empty()) name += " " + last;
    return name;
}

Attachment file_attachment(const nlohmann::json& file, AttachmentKind kind, const std::string& default_name,
                           const std::string& default_mime) {
    Attachment attachment;
    attachment.kind = kind;
    attachment.remote_ref = file.value("file_id", std::string{});
    attachment.name = file.value("file_name", default_name);
    attachment.mime_type = file.value("mime_type", default_mime);
    if (file.contains("file_size") && file["file_size"].is_number_integer() && file["file_size"].get<std::int64_t>() > 0)
        attachment.size = static_cast<std::uint64_t>(file["file_size"].get<std::int64_t>());
    return attachment;
}

std::vector<Attachment> attachments_of(const nlohmann::json& message) {
    constexpr std::uint64_t kPhotoLimit = 20u * 1024u * 1024u;
    std::vector<Attachment> out;
    if (message.contains("photo") && message["photo"].is_array() && !message["photo"].empty()) {
        // 照片有多个尺寸,取不超过下载上限的最大一张。
        const nlohmann::json* best = &message["photo"].back();
        for (const auto& size : message["photo"]) {
            const auto bytes = size.value("file_size", std::int64_t{0});
            if (bytes > 0 && static_cast<std::uint64_t>(bytes) <= kPhotoLimit) best = &size;
        }
        out.push_back(file_attachment(*best, AttachmentKind::Image, "photo.jpg", "image/jpeg"));
    }
    if (message.contains("document") && message["document"].is_object()) {
        auto attachment = file_attachment(message["document"], AttachmentKind::File, "document", "application/octet-stream");
        if (attachment.mime_type.rfind("image/", 0) == 0) attachment.kind = AttachmentKind::Image;
        out.push_back(std::move(attachment));
    }
    const std::pair<const char*, AttachmentKind> unsupported[] = {
        {"voice", AttachmentKind::Voice}, {"audio", AttachmentKind::Voice}, {"video", AttachmentKind::Video},
        {"video_note", AttachmentKind::Video}, {"animation", AttachmentKind::Video},
        {"sticker", AttachmentKind::Sticker}};
    for (const auto& [field, kind] : unsupported) {
        if (message.contains(field) && message[field].is_object())
            out.push_back(file_attachment(message[field], kind, field, ""));
    }
    return out;
}

struct Mention {
    bool mentioned = false;
    std::vector<std::pair<std::size_t, std::size_t>> strip;  // 要从正文删掉的 UTF-8 字节区间
};

Mention find_mention(const std::string& text, const nlohmann::json& entities, const BotIdentity& bot) {
    Mention mention;
    if (!entities.is_array()) return mention;
    const auto handle = "@" + lower(bot.username);
    for (const auto& entity : entities) {
        if (!entity.is_object()) continue;
        const auto type = entity.value("type", std::string{});
        const auto offset = entity.value("offset", std::size_t{0});
        const auto length = entity.value("length", std::size_t{0});
        const auto begin = byte_offset(text, offset);
        const auto end = byte_offset(text, offset + length);
        const auto slice = text.substr(begin, end - begin);
        if (type == "mention" && !bot.username.empty() && lower(slice) == handle) {
            mention.mentioned = true;
            mention.strip.emplace_back(begin, end);
        } else if (type == "text_mention" && entity.contains("user") &&
                   id_string(entity["user"].value("id", nlohmann::json())) == bot.id) {
            mention.mentioned = true;
        } else if (type == "bot_command" && !bot.username.empty()) {
            const auto at = slice.find('@');
            if (at != std::string::npos && lower(slice.substr(at)) == handle) {
                mention.mentioned = true;
                mention.strip.emplace_back(begin + at, end);  // "/status@bot" → "/status"
            }
        }
    }
    return mention;
}

std::string remove_ranges(std::string text, std::vector<std::pair<std::size_t, std::size_t>> ranges) {
    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [begin, end] : ranges) {
        if (begin <= end && end <= text.size()) text.erase(begin, end - begin);
    }
    return text;
}

} // namespace

std::string utf16_slice(const std::string& utf8, std::size_t offset, std::size_t length) {
    const auto begin = byte_offset(utf8, offset);
    const auto end = byte_offset(utf8, offset + length);
    return utf8.substr(begin, end - begin);
}

ParsedUpdate parse_update(const nlohmann::json& update, const BotIdentity& bot, const std::string& account) {
    ParsedUpdate parsed;
    if (!update.is_object()) return parsed;
    parsed.update_id = update.value("update_id", std::int64_t{-1});
    if (!update.contains("message") || !update["message"].is_object()) return parsed;
    const auto& message = update["message"];
    if (!message.contains("chat") || !message.contains("from")) return parsed;
    const auto& chat = message["chat"];
    const auto& from = message["from"];
    if (from.value("is_bot", false)) return parsed;  // 不处理其它机器人(包括自己)的消息
    const auto chat_type = chat.value("type", std::string{});
    if (chat_type != "private" && chat_type != "group" && chat_type != "supergroup") return parsed;

    Inbound inbound;
    inbound.address.platform = "telegram";
    inbound.address.account = account;
    inbound.address.kind = chat_type == "private" ? ChatKind::Private : ChatKind::Group;
    inbound.address.chat = id_string(chat.value("id", nlohmann::json()));
    inbound.address.sender = id_string(from.value("id", nlohmann::json()));
    if (inbound.address.kind == ChatKind::Private) inbound.address.chat = inbound.address.sender;
    if (message.value("is_topic_message", false) && message.contains("message_thread_id"))
        inbound.address.thread = id_string(message["message_thread_id"]);
    if (!inbound.address.valid()) return parsed;
    inbound.message_id = id_string(message.value("message_id", nlohmann::json()));
    if (inbound.message_id.empty()) return parsed;
    inbound.sender_name = display_name(from);

    const bool has_text = message.contains("text") && message["text"].is_string();
    const std::string text = has_text ? message["text"].get<std::string>()
                                      : message.value("caption", std::string{});
    const auto entities = has_text ? message.value("entities", nlohmann::json::array())
                                   : message.value("caption_entities", nlohmann::json::array());
    const auto mention = find_mention(text, entities, bot);
    bool reply_to_bot = false;
    if (message.contains("reply_to_message") && message["reply_to_message"].is_object()) {
        const auto& replied = message["reply_to_message"];
        if (replied.contains("from") && id_string(replied["from"].value("id", nlohmann::json())) == bot.id)
            reply_to_bot = true;
        auto quote = replied.value("text", replied.value("caption", std::string{}));
        if (quote.size() > 4000) quote.resize(4000);
        inbound.quote_text = quote;
    }
    inbound.mentioned = inbound.address.kind == ChatKind::Private || mention.mentioned || reply_to_bot;
    inbound.text = trim(remove_ranges(text, mention.strip));

    if (inbound.address.kind == ChatKind::Private && inbound.text.rfind("/start", 0) == 0) {
        const auto rest = trim(inbound.text.substr(6));
        if (!rest.empty() && rest.find(' ') == std::string::npos) inbound.start_code = rest;
    }
    inbound.attachments = attachments_of(message);
    if (inbound.text.empty() && inbound.attachments.empty()) return parsed;
    inbound.reply_context = {{"message_id", message.value("message_id", std::int64_t{0})}};
    if (!inbound.address.thread.empty()) inbound.reply_context["thread"] = inbound.address.thread;
    parsed.inbound = std::move(inbound);
    return parsed;
}

} // namespace acecode::im::telegram
