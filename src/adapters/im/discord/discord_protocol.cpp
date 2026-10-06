#include "discord_protocol.hpp"

#include "version.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <sstream>

namespace acecode::im::discord {
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

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// 雪花 id 在线上一律是字符串;测试或旧数据里偶尔是数字,一并接受。
std::string string_field(const nlohmann::json& value, const char* key) {
    if (!value.is_object()) return {};
    const auto it = value.find(key);
    if (it == value.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_unsigned()) return std::to_string(it->get<std::uint64_t>());
    if (it->is_number_integer()) return std::to_string(it->get<std::int64_t>());
    return {};
}

std::int64_t int_field(const nlohmann::json& value, const char* key, std::int64_t fallback) {
    if (!value.is_object()) return fallback;
    const auto it = value.find(key);
    if (it == value.end() || !it->is_number()) return fallback;
    if (it->is_number_float()) return static_cast<std::int64_t>(it->get<double>());
    return it->get<std::int64_t>();
}

bool bool_field(const nlohmann::json& value, const char* key) {
    if (!value.is_object()) return false;
    const auto it = value.find(key);
    return it != value.end() && it->is_boolean() && it->get<bool>();
}

const nlohmann::json& object_field(const nlohmann::json& value, const char* key) {
    static const nlohmann::json kNull;
    if (!value.is_object()) return kNull;
    const auto it = value.find(key);
    return it == value.end() ? kNull : *it;
}

bool is_snowflake(const std::string& id) {
    if (id.empty() || id.size() > 24) return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// 展示名:服务器昵称 > 全局显示名 > 用户名。
std::string display_name(const nlohmann::json& user, const nlohmann::json& member) {
    auto nick = string_field(member, "nick");
    if (!nick.empty()) return nick;
    const auto& inner_member = object_field(user, "member");
    nick = string_field(inner_member, "nick");
    if (!nick.empty()) return nick;
    auto global = string_field(user, "global_name");
    if (!global.empty()) return global;
    return string_field(user, "username");
}

struct MentionToken {
    std::size_t begin = 0;
    std::size_t end = 0;  // 不含
    bool role = false;
    std::string id;
};

// 扫描 <@id>、<@!id>(已废弃但仍会出现)与 <@&id>。
std::vector<MentionToken> mention_tokens(const std::string& text) {
    std::vector<MentionToken> out;
    std::size_t i = 0;
    while (i + 2 < text.size()) {
        if (text[i] != '<' || text[i + 1] != '@') {
            ++i;
            continue;
        }
        std::size_t j = i + 2;
        bool role = false;
        if (text[j] == '!') ++j;
        else if (text[j] == '&') {
            role = true;
            ++j;
        }
        const auto digits = j;
        while (j < text.size() && text[j] >= '0' && text[j] <= '9') ++j;
        if (j > digits && j < text.size() && text[j] == '>') {
            out.push_back({i, j + 1, role, text.substr(digits, j - digits)});
            i = j + 1;
            continue;
        }
        ++i;
    }
    return out;
}

std::string mentioned_user_name(const nlohmann::json& mentions, const std::string& id) {
    if (!mentions.is_array()) return {};
    for (const auto& user : mentions) {
        if (string_field(user, "id") == id) return display_name(user, nlohmann::json());
    }
    return {};
}

std::string mime_of(const std::string& content_type) {
    auto mime = content_type;
    const auto semi = mime.find(';');
    if (semi != std::string::npos) mime.resize(semi);
    return lower(trim(mime));
}

Attachment parse_attachment(const nlohmann::json& raw, bool voice_message) {
    Attachment attachment;
    attachment.name = string_field(raw, "filename");
    attachment.mime_type = mime_of(string_field(raw, "content_type"));
    attachment.remote_ref = string_field(raw, "url");
    const auto size = int_field(raw, "size", 0);
    if (size > 0) attachment.size = static_cast<std::uint64_t>(size);
    const auto& mime = attachment.mime_type;
    if (voice_message) attachment.kind = AttachmentKind::Voice;
    else if (starts_with(mime, "image/")) attachment.kind = AttachmentKind::Image;
    else if (starts_with(mime, "video/")) attachment.kind = AttachmentKind::Video;
    else attachment.kind = AttachmentKind::File;
    return attachment;
}

void append_attachments(const nlohmann::json& message, std::vector<Attachment>& out) {
    const auto& list = object_field(message, "attachments");
    if (!list.is_array()) return;
    const bool voice = (int_field(message, "flags", 0) & kMessageFlagVoice) != 0;
    for (const auto& raw : list) {
        if (!raw.is_object()) continue;
        auto attachment = parse_attachment(raw, voice);
        if (!attachment.remote_ref.empty()) out.push_back(std::move(attachment));
    }
}

void flatten_errors(const nlohmann::json& node, const std::string& path, std::vector<std::string>& out) {
    if (!node.is_object()) return;
    const auto errors = node.find("_errors");
    if (errors != node.end() && errors->is_array()) {
        for (const auto& item : *errors) {
            const auto message = string_field(item, "message");
            const auto code = string_field(item, "code");
            std::string line = path.empty() ? std::string{} : path + ": ";
            line += message.empty() ? code : message;
            if (!message.empty() && !code.empty()) line += " (" + code + ")";
            out.push_back(line);
        }
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (it.key() == "_errors") continue;
        flatten_errors(it.value(), path.empty() ? it.key() : path + "." + it.key(), out);
    }
}

bool contains_ci(const std::string& text, const std::string& needle) {
    return lower(text).find(lower(needle)) != std::string::npos;
}

bool is_fence(const std::string& line) {
    const auto t = trim(line);
    return starts_with(t, "```") || starts_with(t, "~~~");
}

bool is_table_delimiter(const std::string& line) {
    const auto t = trim(line);
    if (t.empty() || t.find('-') == std::string::npos) return false;
    if (t.find('|') == std::string::npos) return false;
    return std::all_of(t.begin(), t.end(), [](char c) { return c == '|' || c == '-' || c == ':' || c == ' ' || c == '\t'; });
}

bool is_table_row(const std::string& line) {
    const auto t = trim(line);
    return !t.empty() && t.find('|') != std::string::npos;
}

} // namespace

std::string discord_user_agent() {
    return std::string("DiscordBot (") + kProjectUrl + ", " + ACECODE_VERSION + ")";
}

std::string invite_url(const std::string& application_id) {
    if (!is_snowflake(application_id)) return {};
    return "https://discord.com/oauth2/authorize?client_id=" + application_id +
           "&scope=bot&permissions=" + std::to_string(kInvitePermissions) + "&integration_type=0";
}

std::string gateway_connect_url(const std::string& base) {
    auto url = base;
    const auto query = url.find('?');
    if (query != std::string::npos) url.resize(query);
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.empty()) return {};
    return url + "/?v=10&encoding=json";
}

IntentState message_content_intent(const nlohmann::json& application) {
    const auto& flags = object_field(application, "flags");
    if (!flags.is_number_integer()) return IntentState::Unknown;
    const auto value = flags.get<std::int64_t>();
    return (value & (kAppFlagMessageContent | kAppFlagMessageContentLimited)) != 0 ? IntentState::Enabled
                                                                                  : IntentState::Disabled;
}

ApiError parse_api_error(long status, const std::string& body) {
    ApiError error;
    error.status = status;
    try {
        const auto json = nlohmann::json::parse(body);
        if (json.is_object()) {
            error.code = static_cast<long>(int_field(json, "code", 0));
            error.message = string_field(json, "message");
            const auto& retry = object_field(json, "retry_after");
            if (retry.is_number()) error.retry_after = (std::max)(0.0, retry.get<double>());
            error.global = bool_field(json, "global");
            std::vector<std::string> lines;
            flatten_errors(object_field(json, "errors"), {}, lines);
            for (const auto& line : lines) {
                if (!error.detail.empty()) error.detail += "; ";
                error.detail += line;
            }
        }
    } catch (...) {
    }
    if (error.message.empty()) error.message = "HTTP " + std::to_string(status);
    return error;
}

bool reference_rejected(const ApiError& error) {
    if (error.code == 10008 || error.code == 160002) return true;
    if (error.code != 50035) return false;
    const auto text = error.message + " " + error.detail;
    return contains_ci(text, "system message") || contains_ci(text, "message_reference") ||
           contains_ci(text, "reply");
}

bool upload_too_large(const ApiError& error) { return error.status == 413 || error.code == 40005; }

std::string describe_error(const ApiError& error) {
    if (error.status == 0) {
        return error.message.empty() || starts_with(error.message, "HTTP ") ? "无法连接 Discord"
                                                                             : "无法连接 Discord:" + error.message;
    }
    if (error.status == 401 || error.code == 40001) return "Bot Token 无效或已被重置";
    switch (error.code) {
        case 10003: return "频道不存在,或机器人已无法访问该频道";
        case 10008: return "要回复的消息已不存在";
        case 10013: return "Discord 用户不存在";
        case 20016:
        case 20028: return "Discord 限流,请稍后再试";
        case 40005: return "文件超过 Discord 上传上限";
        case 40058: return "不能直接在论坛频道发消息,请在帖子里 @机器人";
        case 50001: return "机器人无权访问该频道";
        case 50006: return "不能发送空消息";
        case 50007: return "无法私信该用户(对方关闭了私信,或与机器人没有共同的服务器)";
        case 50008: return "不能在该类型的频道发消息";
        case 50013: return "机器人缺少权限(需要发送消息、在子区发送消息、附加文件、读取消息历史)";
        case 160005: return "子区已锁定,机器人无法发言";
        case 50035: return "Discord 拒绝了消息内容:" + (error.detail.empty() ? error.message : error.detail);
        default: break;
    }
    if (error.status == 413) return "文件超过 Discord 上传上限";
    if (error.status == 429) return "Discord 限流,请稍后再试";
    if (error.status == 403) return "请求被 Discord 拒绝(HTTP 403)";
    if (error.status >= 500) return "Discord 服务暂时不可用(HTTP " + std::to_string(error.status) + ")";
    return "Discord 请求失败:" + error.message;
}

std::string render_content(const std::string& content, const std::string& bot_id,
                           const std::set<std::string>& bot_role_ids, const nlohmann::json& mentions,
                           bool* mentioned) {
    bool hit = false;
    std::string out;
    out.reserve(content.size());
    std::size_t pos = 0;
    for (const auto& token : mention_tokens(content)) {
        out.append(content, pos, token.begin - pos);
        pos = token.end;
        const bool self = token.role ? bot_role_ids.count(token.id) > 0 : (!bot_id.empty() && token.id == bot_id);
        if (self) {
            hit = true;
            // 去掉提及后不留双空格:"看看 <@BOT> 这个" → "看看 这个"。
            if (!out.empty() && out.back() == ' ' && pos < content.size() && content[pos] == ' ') ++pos;
            continue;
        }
        const auto name = token.role ? std::string{} : mentioned_user_name(mentions, token.id);
        if (!name.empty()) out += "@" + name;
        else out.append(content, token.begin, token.end - token.begin);
    }
    out.append(content, pos, std::string::npos);
    if (mentioned) *mentioned = hit;
    return trim(out);
}

ParsedMessage parse_message_create(const nlohmann::json& d, const std::string& bot_id,
                                   const std::set<std::string>& bot_role_ids) {
    ParsedMessage parsed;
    if (!d.is_object()) {
        parsed.drop_reason = "malformed payload";
        return parsed;
    }
    const auto message_id = string_field(d, "id");
    const auto channel_id = string_field(d, "channel_id");
    const auto& author = object_field(d, "author");
    const auto author_id = string_field(author, "id");
    if (!is_snowflake(message_id) || !is_snowflake(channel_id) || !is_snowflake(author_id)) {
        parsed.drop_reason = "missing message, channel or author id";
        return parsed;
    }
    if (!bot_id.empty() && author_id == bot_id) {
        parsed.drop_reason = "own message";
        return parsed;
    }
    if (bool_field(author, "bot") || bool_field(author, "system") || !string_field(d, "webhook_id").empty()) {
        parsed.drop_reason = "message from a bot, webhook or system account";
        return parsed;
    }
    // 只接普通消息(0)与回复(19);置顶、加入、子区创建、子区首条(21)等系统类型一律忽略。
    const auto type = int_field(d, "type", 0);
    if (type != 0 && type != 19) {
        parsed.drop_reason = "message type " + std::to_string(type) + " is not a user message";
        return parsed;
    }
    const auto guild_id = string_field(d, "guild_id");
    const auto channel_type = int_field(d, "channel_type", -1);
    if (channel_type == 3) {
        parsed.drop_reason = "group DM";
        return parsed;
    }
    // channel_type 可能缺失(旧载荷):没有 guild_id 就是私聊。
    const bool dm = channel_type == 1 || guild_id.empty();

    Inbound inbound;
    inbound.message_id = message_id;
    inbound.address.platform = kPlatform;
    inbound.address.account = bot_id;
    inbound.address.sender = author_id;
    inbound.address.kind = dm ? ChatKind::Private : ChatKind::Group;
    inbound.address.chat = dm ? author_id : channel_id;

    const auto& mentions = object_field(d, "mentions");
    bool inline_mention = false;
    auto content = string_field(d, "content");
    // 转发的消息:外层正文为空时用快照里的正文,附件也一起带上。
    std::vector<Attachment> attachments;
    append_attachments(d, attachments);
    const auto& snapshots = object_field(d, "message_snapshots");
    if (snapshots.is_array()) {
        for (const auto& snapshot : snapshots) {
            const auto& inner = object_field(snapshot, "message");
            if (!inner.is_object()) continue;
            if (trim(content).empty()) content = string_field(inner, "content");
            append_attachments(inner, attachments);
        }
    }
    inbound.text = render_content(content, bot_id, bot_role_ids, mentions, &inline_mention);

    bool mentioned = inline_mention;
    if (!bot_id.empty() && mentions.is_array()) {
        for (const auto& user : mentions) mentioned = mentioned || string_field(user, "id") == bot_id;
    }
    const auto& mention_roles = object_field(d, "mention_roles");
    if (mention_roles.is_array()) {
        for (const auto& role : mention_roles) {
            if (role.is_string() && bot_role_ids.count(role.get<std::string>())) mentioned = true;
        }
    }
    // 回复机器人的消息也算点名(用户关掉回复里的 @ 时 mentions 里没有机器人,要看被回复消息的作者)。
    const auto& referenced = object_field(d, "referenced_message");
    if (referenced.is_object()) {
        const auto& ref_author = object_field(referenced, "author");
        if (!bot_id.empty() && string_field(ref_author, "id") == bot_id) mentioned = true;
        inbound.quote_text = render_content(string_field(referenced, "content"), bot_id, bot_role_ids,
                                            object_field(referenced, "mentions"));
        append_attachments(referenced, attachments);
    }
    inbound.mentioned = dm ? true : mentioned;
    inbound.attachments = std::move(attachments);
    if (inbound.text.empty() && inbound.attachments.empty()) {
        parsed.drop_reason = inline_mention ? "bare mention without text" : "empty message";
        return parsed;
    }
    inbound.sender_name = display_name(author, object_field(d, "member"));
    inbound.reply_context = {{"message_id", message_id}, {"channel_id", channel_id}};
    if (!guild_id.empty()) inbound.reply_context["guild_id"] = guild_id;
    if (dm) {
        parsed.dm_user = author_id;
        parsed.dm_channel = channel_id;
    }
    parsed.inbound = std::move(inbound);
    return parsed;
}

std::vector<std::string> bot_role_ids_in_guild(const nlohmann::json& guild, const std::string& bot_id) {
    std::vector<std::string> out;
    if (bot_id.empty()) return out;
    const auto& roles = object_field(guild, "roles");
    if (!roles.is_array()) return out;
    for (const auto& role : roles) {
        const auto& tags = object_field(role, "tags");
        if (string_field(tags, "bot_id") == bot_id) {
            const auto id = string_field(role, "id");
            if (is_snowflake(id)) out.push_back(id);
        }
    }
    return out;
}

std::string format_markdown(const std::string& markdown) {
    std::vector<std::string> lines;
    {
        std::size_t start = 0;
        while (true) {
            const auto nl = markdown.find('\n', start);
            if (nl == std::string::npos) {
                lines.push_back(markdown.substr(start));
                break;
            }
            lines.push_back(markdown.substr(start, nl - start));
            start = nl + 1;
        }
    }
    std::vector<std::string> out;
    out.reserve(lines.size());
    bool in_fence = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        if (is_fence(line)) {
            in_fence = !in_fence;
            out.push_back(line);
            continue;
        }
        if (in_fence) {
            out.push_back(line);
            continue;
        }
        // GFM 表格:表头行 + 分隔行(|---|:--:|),之后连续的含 '|' 行都属于表格。
        if (is_table_row(line) && i + 1 < lines.size() && is_table_delimiter(lines[i + 1])) {
            out.push_back("```");
            std::size_t j = i;
            while (j < lines.size() && is_table_row(lines[j]) && !is_fence(lines[j])) {
                out.push_back(lines[j]);
                ++j;
            }
            out.push_back("```");
            i = j - 1;
            continue;
        }
        // Discord 只渲染 #、##、### 三级标题,更深的标题会显示成带井号的原文。
        const auto t = trim(line);
        if (starts_with(t, "####")) {
            std::size_t hashes = 0;
            while (hashes < t.size() && t[hashes] == '#') ++hashes;
            if (hashes <= 6 && hashes < t.size() && (t[hashes] == ' ' || t[hashes] == '\t')) {
                const auto text = trim(t.substr(hashes));
                out.push_back(text.empty() ? std::string{} : "**" + text + "**");
                continue;
            }
        }
        out.push_back(line);
    }
    std::string result;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i) result.push_back('\n');
        result += out[i];
    }
    return result;
}

std::string url_host(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    auto rest = url.substr(scheme + 3);
    const auto end = rest.find_first_of("/?#");
    if (end != std::string::npos) rest.resize(end);
    const auto at = rest.rfind('@');
    if (at != std::string::npos) rest = rest.substr(at + 1);
    if (!rest.empty() && rest.front() == '[') {
        const auto close = rest.find(']');
        return close == std::string::npos ? std::string{} : lower(rest.substr(1, close - 1));
    }
    const auto colon = rest.find(':');
    if (colon != std::string::npos) rest.resize(colon);
    return lower(rest);
}

std::string attachment_id_from_url(const std::string& url) {
    static const std::string kMarker = "/attachments/";
    const auto pos = url.find(kMarker);
    if (pos == std::string::npos) return {};
    const auto channel_end = url.find('/', pos + kMarker.size());
    if (channel_end == std::string::npos) return {};
    const auto id_end = url.find('/', channel_end + 1);
    if (id_end == std::string::npos) return {};
    const auto id = url.substr(channel_end + 1, id_end - channel_end - 1);
    return is_snowflake(id) ? id : std::string{};
}

} // namespace acecode::im::discord
