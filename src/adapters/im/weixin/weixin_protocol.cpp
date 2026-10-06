#include "weixin_protocol.hpp"

#include "im/text_chunk.hpp"
#include "platform/crypto/digest.hpp"
#include "utils/base64.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace acecode::im::weixin {
namespace {

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

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string string_field(const nlohmann::json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_string()) return {};
    return object[key].get<std::string>();
}

const nlohmann::json& object_field(const nlohmann::json& object, const char* key) {
    static const nlohmann::json kEmpty = nlohmann::json::object();
    if (!object.is_object() || !object.contains(key) || !object[key].is_object()) return kEmpty;
    return object[key];
}

std::int64_t int64_field(const nlohmann::json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) return 0;
    const auto& value = object[key];
    if (value.is_number_unsigned()) {
        const auto v = value.get<std::uint64_t>();
        return v > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
                   ? (std::numeric_limits<std::int64_t>::max)()
                   : static_cast<std::int64_t>(v);
    }
    if (value.is_number_integer()) return value.get<std::int64_t>();
    if (value.is_string()) {
        try {
            std::size_t used = 0;
            const auto text = value.get<std::string>();
            const auto v = std::stoll(text, &used);
            return used == text.size() ? v : 0;
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

int int_field(const nlohmann::json& object, const char* key) {
    const auto value = int64_field(object, key);
    if (value > (std::numeric_limits<int>::max)()) return (std::numeric_limits<int>::max)();
    if (value < (std::numeric_limits<int>::min)()) return (std::numeric_limits<int>::min)();
    return static_cast<int>(value);
}

bool is_hex(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

bool is_media_type(int type) {
    return type == kItemImage || type == kItemVoice || type == kItemFile || type == kItemVideo;
}

const char* media_label(int type) {
    switch (type) {
        case kItemImage: return "图片";
        case kItemVoice: return "语音";
        case kItemFile: return "文件";
        case kItemVideo: return "视频";
        default: return "媒体";
    }
}

std::optional<std::string> image_key(const nlohmann::json& image_item, bool& invalid) {
    // 图片优先用 image_item.aeskey(32 个十六进制字符),其次 media.aes_key。
    const auto hex = string_field(image_item, "aeskey");
    if (!hex.empty()) {
        if (auto key = hex.size() == 32 ? hex_decode(hex) : std::nullopt) return key;
        invalid = true;
        return std::nullopt;
    }
    const auto encoded = string_field(object_field(image_item, "media"), "aes_key");
    if (encoded.empty()) return std::string{};
    if (auto key = parse_aes_key(encoded)) return key;
    invalid = true;
    return std::nullopt;
}

std::optional<std::string> media_key(const nlohmann::json& media, bool& invalid) {
    const auto encoded = string_field(media, "aes_key");
    if (encoded.empty()) return std::string{};
    if (auto key = parse_aes_key(encoded)) return key;
    invalid = true;
    return std::nullopt;
}

// 一个媒体条目 → 附件;不是媒体条目时返回空。
std::optional<Attachment> media_attachment(const nlohmann::json& item) {
    const int type = int_field(item, "type");
    if (!is_media_type(type)) return std::nullopt;
    Attachment attachment;
    MediaRef ref;
    const nlohmann::json* body = nullptr;
    std::optional<std::string> key;
    switch (type) {
        case kItemImage:
            body = &object_field(item, "image_item");
            key = image_key(*body, ref.key_invalid);
            attachment.kind = AttachmentKind::Image;
            attachment.name = "image.jpg";
            attachment.mime_type = "image/jpeg";
            ref.kind = "image";
            break;
        case kItemVoice:
            body = &object_field(item, "voice_item");
            key = media_key(object_field(*body, "media"), ref.key_invalid);
            attachment.kind = AttachmentKind::Voice;
            attachment.name = "voice.silk";
            attachment.mime_type = "audio/silk";
            attachment.transcript = trim(string_field(*body, "text"));
            ref.kind = "voice";
            break;
        case kItemFile: {
            body = &object_field(item, "file_item");
            key = media_key(object_field(*body, "media"), ref.key_invalid);
            const auto name = trim(string_field(*body, "file_name"));
            attachment.name = name.empty() ? std::string("document.bin") : name;
            attachment.mime_type = guess_mime(attachment.name);
            attachment.kind = attachment.mime_type.rfind("image/", 0) == 0 && attachment.mime_type != "image/svg+xml"
                                  ? AttachmentKind::Image
                                  : AttachmentKind::File;
            const auto size = int64_field(*body, "len");
            if (size > 0) attachment.size = static_cast<std::uint64_t>(size);
            ref.kind = "file";
            break;
        }
        default:
            body = &object_field(item, "video_item");
            key = media_key(object_field(*body, "media"), ref.key_invalid);
            attachment.kind = AttachmentKind::Video;
            attachment.name = "video.mp4";
            attachment.mime_type = "video/mp4";
            ref.kind = "video";
            break;
    }
    const auto& media = object_field(*body, "media");
    ref.query_param = string_field(media, "encrypt_query_param");
    ref.full_url = string_field(media, "full_url");
    if (key) ref.aes_key = *key;
    if (!ref.query_param.empty() || !ref.full_url.empty()) attachment.remote_ref = encode_media_ref(ref);
    return attachment;
}

// 条目的文字(引用里的文字条目会递归取到它自己的正文)。
std::string item_text(const nlohmann::json& item) {
    if (int_field(item, "type") == kItemText) return string_field(object_field(item, "text_item"), "text");
    if (int_field(item, "type") == kItemVoice) return trim(string_field(object_field(item, "voice_item"), "text"));
    return {};
}

std::string quote_of(const nlohmann::json& ref_msg) {
    const auto title = trim(string_field(ref_msg, "title"));
    const auto& quoted = object_field(ref_msg, "message_item");
    if (!quoted.empty()) {
        const int type = int_field(quoted, "type");
        if (is_media_type(type)) {
            std::string label = std::string("[") + media_label(type) + "]";
            if (type == kItemFile) {
                const auto name = trim(string_field(object_field(quoted, "file_item"), "file_name"));
                if (!name.empty()) label += " " + name;
            }
            return title.empty() ? label : label + " " + title;
        }
        const auto text = trim(item_text(quoted));
        if (!title.empty() && !text.empty() && title != text) return title + " | " + text;
        return text.empty() ? title : text;
    }
    return title;
}

struct Authority {
    std::string scheme;
    std::string host;  // 小写,不含端口
    std::string port;  // 可空
};

std::optional<Authority> parse_authority(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return std::nullopt;
    Authority authority;
    authority.scheme = lower(url.substr(0, scheme_end));
    const auto rest = url.substr(scheme_end + 3);
    const auto end = rest.find_first_of("/?#");
    const auto hostport = rest.substr(0, end);
    if (hostport.empty() || hostport.find('@') != std::string::npos) return std::nullopt;
    const auto colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(']') == std::string::npos) {
        authority.host = lower(hostport.substr(0, colon));
        authority.port = hostport.substr(colon + 1);
    } else {
        authority.host = lower(hostport);
    }
    if (authority.host.empty()) return std::nullopt;
    return authority;
}

std::string json_text(const nlohmann::json& value) {
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace

std::uint32_t client_version_number(const std::string& version) {
    std::uint32_t parts[3] = {0, 0, 0};
    std::size_t index = 0, pos = 0;
    while (index < 3 && pos <= version.size()) {
        const auto dot = version.find('.', pos);
        const auto piece = version.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
        try {
            parts[index] = static_cast<std::uint32_t>(std::stoul(piece)) & 0xFFu;
        } catch (...) {
            parts[index] = 0;
        }
        ++index;
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    return (parts[0] << 16) | (parts[1] << 8) | parts[2];
}

std::string wechat_uin(std::uint32_t value) { return base64_encode(std::to_string(value)); }

std::string join_url(const std::string& base, const std::string& path) {
    std::string left = base;
    while (!left.empty() && left.back() == '/') left.pop_back();
    std::size_t start = 0;
    while (start < path.size() && path[start] == '/') ++start;
    return left + "/" + path.substr(start);
}

std::string url_encode(const std::string& value) {
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

Envelope parse_envelope(const std::string& body) {
    Envelope envelope;
    try {
        auto json = nlohmann::json::parse(body);
        if (!json.is_object()) return envelope;
        envelope.parsed = true;
        envelope.ret = int_field(json, "ret");
        envelope.errcode = int_field(json, "errcode");
        envelope.errmsg = string_field(json, "errmsg");
        envelope.body = std::move(json);
    } catch (...) {
    }
    return envelope;
}

bool envelope_failed(const Envelope& envelope) {
    return !envelope.parsed || envelope.ret != 0 || envelope.errcode != 0;
}

bool is_session_expired(int ret, int errcode, const std::string& errmsg) {
    if (ret == -14 || errcode == -14) return true;
    return (ret == -2 || errcode == -2) && lower(trim(errmsg)) == "unknown error";
}

bool is_rate_limited(int ret, int errcode, const std::string& errmsg) {
    if (ret != -2 && errcode != -2) return false;
    if (is_session_expired(ret, errcode, errmsg)) return false;
    const auto message = lower(errmsg);
    return message.find("param") == std::string::npos && message.find("invalid") == std::string::npos;
}

std::string id_string(const nlohmann::json& value) {
    if (value.is_number_unsigned()) return std::to_string(value.get<std::uint64_t>());
    if (value.is_number_integer()) return std::to_string(value.get<std::int64_t>());
    if (value.is_string()) return value.get<std::string>();
    return {};
}

bool valid_id(const std::string& value) {
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
               c == '.' || c == ':' || c == '@';
    });
}

std::string encode_media_ref(const MediaRef& ref) {
    nlohmann::json value{{"v", 1}, {"t", ref.kind}};
    if (!ref.query_param.empty()) value["q"] = ref.query_param;
    if (!ref.full_url.empty()) value["u"] = ref.full_url;
    if (!ref.aes_key.empty()) value["k"] = platform::to_hex(ref.aes_key);
    if (ref.key_invalid) value["x"] = true;
    return json_text(value);
}

std::optional<MediaRef> decode_media_ref(const std::string& text) {
    try {
        const auto value = nlohmann::json::parse(text);
        if (!value.is_object() || value.value("v", 0) != 1) return std::nullopt;
        MediaRef ref;
        ref.kind = string_field(value, "t");
        ref.query_param = string_field(value, "q");
        ref.full_url = string_field(value, "u");
        ref.key_invalid = value.value("x", false);
        const auto hex = string_field(value, "k");
        if (!hex.empty()) {
            auto key = hex_decode(hex);
            if (!key || key->size() != 16) return std::nullopt;
            ref.aes_key = *key;
        }
        if (ref.query_param.empty() && ref.full_url.empty()) return std::nullopt;
        return ref;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> hex_decode(const std::string& hex) {
    if (hex.size() % 2 != 0 || !is_hex(hex)) return std::nullopt;
    const auto nibble = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
    };
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2)
        out.push_back(static_cast<char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    return out;
}

std::optional<std::string> parse_aes_key(const std::string& value) {
    const auto decoded = base64_decode(trim(value));
    if (!decoded) return std::nullopt;
    if (decoded->size() == 16) return decoded;
    if (decoded->size() == 32 && is_hex(*decoded)) return hex_decode(*decoded);
    return std::nullopt;
}

std::string outbound_aes_key(const std::string& raw_key) { return base64_encode(platform::to_hex(raw_key)); }

std::string strip_pkcs7_lenient(std::string data) {
    if (data.empty()) return data;
    const auto pad = static_cast<unsigned char>(data.back());
    if (pad < 1 || pad > 16 || pad > data.size()) return data;
    for (std::size_t i = data.size() - pad; i < data.size(); ++i)
        if (static_cast<unsigned char>(data[i]) != pad) return data;
    data.resize(data.size() - pad);
    return data;
}

std::string guess_mime(const std::string& file_name) {
    static const std::pair<const char*, const char*> kTypes[] = {
        {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"},
        {".webp", "image/webp"}, {".bmp", "image/bmp"}, {".svg", "image/svg+xml"},
        {".pdf", "application/pdf"}, {".txt", "text/plain"}, {".md", "text/markdown"}, {".csv", "text/csv"},
        {".json", "application/json"}, {".html", "text/html"}, {".htm", "text/html"},
        {".zip", "application/zip"}, {".7z", "application/x-7z-compressed"}, {".rar", "application/vnd.rar"},
        {".doc", "application/msword"},
        {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {".xls", "application/vnd.ms-excel"},
        {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {".ppt", "application/vnd.ms-powerpoint"},
        {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {".mp4", "video/mp4"}, {".mov", "video/quicktime"}, {".mp3", "audio/mpeg"}, {".wav", "audio/wav"},
        {".ogg", "audio/ogg"}, {".amr", "audio/amr"}, {".silk", "audio/silk"},
    };
    const auto name = lower(file_name);
    for (const auto& [ext, mime] : kTypes)
        if (ends_with(name, ext)) return mime;
    return "application/octet-stream";
}

UploadMediaType upload_media_type(const std::string& mime_type, const std::string& file_name) {
    const auto mime = mime_type.empty() || mime_type == "application/octet-stream" ? guess_mime(file_name)
                                                                                   : lower(mime_type);
    if (mime.rfind("image/", 0) == 0 && mime != "image/svg+xml") return UploadMediaType::Image;
    if (mime.rfind("video/", 0) == 0) return UploadMediaType::Video;
    return UploadMediaType::File;
}

std::string cdn_download_url(const std::string& cdn_base, const std::string& query_param) {
    return join_url(cdn_base, "download") + "?encrypted_query_param=" + url_encode(query_param);
}

std::string cdn_upload_url(const std::string& cdn_base, const std::string& upload_param, const std::string& filekey) {
    return join_url(cdn_base, "upload") + "?encrypted_query_param=" + url_encode(upload_param) +
           "&filekey=" + url_encode(filekey);
}

bool media_host_allowed(const std::string& url, const std::string& cdn_base) {
    static const char* kHosts[] = {"novac2c.cdn.weixin.qq.com", "ilinkai.weixin.qq.com", "wx.qlogo.cn",
                                   "thirdwx.qlogo.cn",          "res.wx.qq.com",          "mmbiz.qpic.cn",
                                   "mmbiz.qlogo.cn"};
    const auto target = parse_authority(url);
    if (!target || (target->scheme != "https" && target->scheme != "http")) return false;
    for (const char* host : kHosts)
        if (target->host == host) return true;
    const auto cdn = parse_authority(cdn_base);
    return cdn && cdn->scheme == target->scheme && cdn->host == target->host && cdn->port == target->port;
}

ParsedMessage parse_message(const nlohmann::json& message, const std::string& account) {
    ParsedMessage parsed;
    if (!message.is_object()) {
        parsed.drop_reason = "not an object";
        return parsed;
    }
    parsed.sender = string_field(message, "from_user_id");
    parsed.message_id = id_string(message.value("message_id", nlohmann::json()));
    if (parsed.message_id.empty()) parsed.message_id = string_field(message, "client_id");
    if (parsed.message_id.empty() && message.contains("seq"))
        parsed.message_id = "seq-" + id_string(message["seq"]);
    parsed.context_token = string_field(message, "context_token");
    parsed.create_time_ms = int64_field(message, "create_time_ms");
    if (parsed.sender.empty()) {
        parsed.drop_reason = "missing sender";
        return parsed;
    }
    if (parsed.sender == account || int_field(message, "message_type") == kMessageTypeBot) {
        parsed.drop_reason = "bot message";
        return parsed;
    }
    if (!valid_id(parsed.sender)) {
        parsed.drop_reason = "invalid sender id";
        return parsed;
    }
    parsed.from_user = true;
    if (parsed.message_id.empty()) {
        parsed.drop_reason = "missing message id";
        return parsed;
    }

    Inbound inbound;
    inbound.address.platform = kPlatform;
    inbound.address.account = account;
    inbound.address.kind = ChatKind::Private;
    inbound.address.chat = parsed.sender;
    inbound.address.sender = parsed.sender;
    inbound.message_id = parsed.message_id;
    inbound.mentioned = true;

    static const nlohmann::json kNoItems = nlohmann::json::array();
    const auto& items = message.contains("item_list") && message["item_list"].is_array() ? message["item_list"]
                                                                                       : kNoItems;
    bool text_found = false;
    std::vector<Attachment> quoted;
    for (const auto& item : items) {
        if (!item.is_object()) continue;
        if (!text_found && int_field(item, "type") == kItemText) {
            text_found = true;
            inbound.text = trim(string_field(object_field(item, "text_item"), "text"));
            const auto& ref = object_field(item, "ref_msg");
            if (!ref.empty()) {
                inbound.quote_text = quote_of(ref);
                if (inbound.quote_text.size() > 4000) inbound.quote_text.resize(4000);
            }
        }
        if (auto attachment = media_attachment(item)) inbound.attachments.push_back(std::move(*attachment));
        const auto& ref_item = object_field(object_field(item, "ref_msg"), "message_item");
        if (!ref_item.empty()) {
            if (auto attachment = media_attachment(ref_item)) quoted.push_back(std::move(*attachment));
        }
    }
    // 被引用的媒体排在本条消息自己的附件之后(“看看这张图”引用旧图片时也能拿到它)。
    for (auto& attachment : quoted) inbound.attachments.push_back(std::move(attachment));
    if (inbound.text.empty() && inbound.attachments.empty()) {
        parsed.drop_reason = "no content";
        return parsed;
    }
    inbound.reply_context = {{"message_id", parsed.message_id}};
    parsed.inbound = std::move(inbound);
    return parsed;
}

nlohmann::json text_item(const std::string& text) {
    return {{"type", kItemText}, {"text_item", {{"text", text}}}};
}

nlohmann::json media_item(UploadMediaType type, const std::string& encrypt_query_param, const std::string& raw_key,
                          std::uint64_t ciphertext_size, std::uint64_t plaintext_size, const std::string& md5_hex,
                          const std::string& file_name) {
    const nlohmann::json media{{"encrypt_query_param", encrypt_query_param},
                               {"aes_key", outbound_aes_key(raw_key)},
                               {"encrypt_type", 1}};
    switch (type) {
        case UploadMediaType::Image:
            return {{"type", kItemImage}, {"image_item", {{"media", media}, {"mid_size", ciphertext_size}}}};
        case UploadMediaType::Video:
            return {{"type", kItemVideo},
                    {"video_item", {{"media", media}, {"video_size", ciphertext_size}, {"play_length", 0},
                                    {"video_md5", md5_hex}}}};
        case UploadMediaType::File:
        default:
            return {{"type", kItemFile},
                    {"file_item", {{"media", media}, {"file_name", file_name},
                                   {"len", std::to_string(plaintext_size)}}}};
    }
}

nlohmann::json build_send_body(const std::string& to, const std::string& client_id, const nlohmann::json& item,
                               const std::string& context_token) {
    nlohmann::json msg{{"from_user_id", ""},
                       {"to_user_id", to},
                       {"client_id", client_id},
                       {"message_type", kMessageTypeBot},
                       {"message_state", 2},
                       {"item_list", nlohmann::json::array({item})}};
    if (!context_token.empty()) msg["context_token"] = context_token;
    return {{"msg", std::move(msg)}};
}

FragmentMerger::FragmentMerger(MergeLimits limits) : limits_(limits) {}

std::vector<Inbound> FragmentMerger::push(Inbound inbound, Clock::time_point now) {
    std::vector<Inbound> ready;
    const auto key = inbound.address.sender;
    const bool mergeable = inbound.attachments.empty() && !inbound.text.empty();
    const bool long_fragment = mergeable && text_units(inbound.text, false) >= limits_.split_threshold;
    auto it = pending_.find(key);
    if (it != pending_.end()) {
        if (!mergeable) {
            ready.push_back(std::move(it->second.inbound));
            pending_.erase(it);
            ready.push_back(std::move(inbound));
            return ready;
        }
        // 片段按到达顺序用换行拼接(与 hermes 一致);消息 id 与回复定位沿用第一段。
        it->second.inbound.text += "\n" + inbound.text;
        if (long_fragment) {
            it->second.deadline = now + limits_.wait;
            return ready;
        }
        ready.push_back(std::move(it->second.inbound));
        pending_.erase(it);
        return ready;
    }
    if (long_fragment) {
        pending_.emplace(key, Pending{std::move(inbound), now + limits_.wait});
        return ready;
    }
    ready.push_back(std::move(inbound));
    return ready;
}

std::vector<Inbound> FragmentMerger::take_due(Clock::time_point now) {
    std::vector<std::pair<Clock::time_point, std::string>> due;
    for (const auto& [key, pending] : pending_)
        if (pending.deadline <= now) due.emplace_back(pending.deadline, key);
    std::sort(due.begin(), due.end());
    std::vector<Inbound> ready;
    for (const auto& entry : due) {
        auto it = pending_.find(entry.second);
        ready.push_back(std::move(it->second.inbound));
        pending_.erase(it);
    }
    return ready;
}

std::vector<Inbound> FragmentMerger::take_all() {
    return take_due(Clock::time_point::max());
}

InboundDeduper::InboundDeduper(std::size_t capacity, std::chrono::milliseconds content_window)
    : capacity_(capacity == 0 ? 1 : capacity), window_(content_window) {}

bool InboundDeduper::remember(const std::string& key) {
    if (keys_.count(key)) return true;
    keys_.insert(key);
    order_.push_back(key);
    while (order_.size() > capacity_) {
        keys_.erase(order_.front());
        order_.pop_front();
    }
    return false;
}

bool InboundDeduper::seen_id(const std::string& message_id) {
    if (message_id.empty()) return false;
    return remember("id:" + message_id);
}

bool InboundDeduper::seen_content(const std::string& sender, const std::string& text, std::int64_t create_time_ms,
                                  Clock::time_point now) {
    if (text.empty()) return false;  // 纯媒体消息不按内容去重,连发两张图都要收到
    const auto digest = platform::to_hex(platform::md5_digest(text));
    const auto key = "content:" + sender + ":" + digest;
    if (create_time_ms > 0) return remember(key + ":" + std::to_string(create_time_ms));
    for (auto it = recent_.begin(); it != recent_.end();) {
        if (now - it->second > window_) it = recent_.erase(it);
        else ++it;
    }
    const auto found = recent_.find(key);
    const bool duplicate = found != recent_.end();
    recent_[key] = now;
    return duplicate;
}

} // namespace acecode::im::weixin
