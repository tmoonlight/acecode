#include "im/feishu/feishu_protocol.hpp"

#include "im/feishu/feishu_text_util.hpp"

#include <algorithm>
#include <limits>

namespace acecode::im::feishu {

using namespace detail;

namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percent_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hex_value(text[i + 1]);
            const int lo = hex_value(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string code_text(std::int64_t code) { return "(错误码 " + std::to_string(code) + ")"; }

} // namespace

std::string base_for_domain(const std::string& domain) {
    const auto value = lower(trim(domain));
    return value == kDomainLark || value == "larksuite" ? kLarkBase : kFeishuBase;
}

bool apply_client_config(const nlohmann::json& raw, ClientConfig* config) {
    if (!raw.is_object() || !config) return false;
    bool applied = false;
    const auto read = [&raw](const char* key, std::int64_t* out) {
        if (!raw.contains(key)) return false;
        const auto& field = raw.at(key);
        if (!field.is_number()) return false;
        *out = field.is_number_float() ? static_cast<std::int64_t>(field.get<double>()) : field.get<std::int64_t>();
        return true;
    };
    constexpr std::int64_t kMaxSeconds = 24 * 3600;
    std::int64_t value = 0;
    if (read("ReconnectCount", &value)) {
        config->reconnect_count = static_cast<int>(std::clamp<std::int64_t>(value, -1, 1000000));
        applied = true;
    }
    if (read("ReconnectInterval", &value) && value >= 0) {
        config->reconnect_interval_s = static_cast<int>(std::min(value, kMaxSeconds));
        applied = true;
    }
    if (read("ReconnectNonce", &value) && value >= 0) {
        config->reconnect_nonce_s = static_cast<int>(std::min(value, kMaxSeconds));
        applied = true;
    }
    if (read("PingInterval", &value) && value > 0) {
        config->ping_interval_s = static_cast<int>(std::min(value, kMaxSeconds));
        applied = true;
    }
    return applied;
}

std::string url_query_param(const std::string& url, const std::string& name) {
    const auto question = url.find('?');
    if (question == std::string::npos) return {};
    auto query = std::string_view(url).substr(question + 1);
    const auto hash = query.find('#');
    if (hash != std::string_view::npos) query = query.substr(0, hash);
    std::size_t start = 0;
    while (start <= query.size()) {
        const auto end = query.find('&', start);
        const auto pair = query.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        const auto eq = pair.find('=');
        const auto key = percent_decode(pair.substr(0, eq));
        if (key == name) return eq == std::string_view::npos ? std::string{} : percent_decode(pair.substr(eq + 1));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return {};
}

EndpointInfo parse_endpoint_response(long status, const std::string& body) {
    EndpointInfo info;
    info.status = status;
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(body);
    } catch (...) {
        json = nullptr;
    }
    const auto msg = json.is_object() ? string_field(json, "msg") : std::string{};
    if (status != 200) {
        info.message = "获取飞书长连接地址失败(HTTP " + std::to_string(status) + ")";
        return info;
    }
    if (!json.is_object() || !json.contains("code")) {
        info.message = "飞书长连接接口返回了无效的数据";
        return info;
    }
    info.code = int_field(json, "code", -1);
    if (info.code == 0) {
        const auto& data = object_field(json, "data");
        info.url = string_field(data, "URL");
        if (data.contains("ClientConfig") && data["ClientConfig"].is_object()) info.client_config = data["ClientConfig"];
        if (info.url.empty()) {
            info.message = "飞书没有返回长连接地址";
            return info;
        }
        info.device_id = url_query_param(info.url, "device_id");
        const auto service = url_query_param(info.url, "service_id");
        const auto parsed = int_field(nlohmann::json{{"v", service}}, "v", std::numeric_limits<std::int64_t>::min());
        if (parsed < std::numeric_limits<std::int32_t>::min() || parsed > std::numeric_limits<std::int32_t>::max()) {
            info.url.clear();
            info.message = "飞书返回的长连接地址缺少 service_id";
            return info;
        }
        info.service_id = static_cast<std::int32_t>(parsed);
        info.ok = true;
        return info;
    }
    if (info.code == 1 || info.code == 1000040343) {
        info.message = "飞书长连接服务繁忙,稍后重试";
        return info;
    }
    info.fatal = true;
    if (info.code == 1000040345)
        info.message = "App ID 或 App Secret 无效(也请确认选择的是飞书还是 Lark)";
    else if (info.code == 1000040344)
        info.message = "缺少 App ID 或 App Secret";
    else
        info.message = "飞书拒绝了长连接:" + (msg.empty() ? std::string("未知原因") : msg) + code_text(info.code);
    return info;
}

HandshakeDecision classify_handshake(long http_status, int handshake_status, std::int64_t auth_errcode) {
    HandshakeDecision decision;
    if (handshake_status == 403) {
        decision.fatal = true;
        decision.message = "飞书拒绝了长连接(403),请确认应用为企业自建应用且凭据正确";
    } else if (handshake_status == 514 && auth_errcode == 1000040350) {
        decision.fatal = true;
        decision.message = "该应用的长连接数已达上限,请关闭在其它地方运行的同一机器人";
    } else {
        decision.message = "无法建立飞书长连接" +
                           (http_status > 0 ? "(HTTP " + std::to_string(http_status) + ")" : std::string{}) +
                           ",稍后重试";
    }
    return decision;
}

ApiResult parse_api_response(long status, const std::string& body) {
    ApiResult result;
    result.status = status;
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(body);
    } catch (...) {
        json = nullptr;
    }
    if (json.is_object()) {
        result.body = json;
        result.code = int_field(json, "code", 0);
        result.msg = string_field(json, "msg");
        if (json.contains("data") && json["data"].is_object()) result.data = json["data"];
    }
    result.ok = status >= 200 && status < 300 && json.is_object() && result.code == 0;
    return result;
}

bool is_token_invalid(std::int64_t code) {
    return code == 99991661 || code == 99991663 || code == 99991664 || code == 99991665;
}

bool is_rate_limited(const ApiResult& result) {
    const auto code = result.code;
    return result.status == 429 || code == 99991400 || code == 99991402 || code == 11020 || code == 11021 ||
           code == 230020;
}

std::chrono::seconds parse_retry_after(const std::string& value) {
    const auto text = trim(value);
    if (text.empty() || text.size() > 9) return std::chrono::seconds(0);
    long long seconds = 0;
    for (const char c : text) {
        if (!is_digit(c)) return std::chrono::seconds(0);
        seconds = seconds * 10 + (c - '0');
    }
    return std::chrono::seconds(std::min<long long>(seconds, 3600));
}

std::chrono::milliseconds rate_limit_wait(const ApiResult& result, std::chrono::milliseconds fallback) {
    const auto requested = std::min<std::chrono::milliseconds>(result.retry_after, std::chrono::seconds(60));
    return std::max(fallback, requested);
}

bool is_transient(const ApiResult& result) {
    if (result.cancelled || result.auth_failed) return false;
    if (result.status == 0 || result.status >= 500) return true;
    return result.code >= 50000 && result.code <= 59999;
}

bool is_post_rejected(const ApiResult& result) {
    if (result.ok) return false;
    return result.code == 230001 ||
           lower(result.msg).find("content format of the post type is incorrect") != std::string::npos;
}

bool is_reply_target_gone(std::int64_t code) { return code == 230011 || code == 231003; }

std::string describe_error(const ApiResult& result) {
    if (result.cancelled) return "已取消";
    if (result.auth_failed) return result.error.empty() ? std::string("飞书凭据无效") : result.error;
    if (result.status == 0) {
        return "无法连接飞书开放平台" + (result.error.empty() ? std::string{} : ":" + result.error);
    }
    if (is_rate_limited(result)) return "发送过于频繁,被飞书限流";
    if (is_token_invalid(result.code)) return "飞书访问凭证失效,请稍后重试";
    switch (result.code) {
        case 99991401: return "请求被应用的 IP 白名单拒绝,请在开发者后台「安全设置」中检查";
        case 99991403: return "应用本月的 API 调用额度已用完";
        case 99991662:
        case 99991673: return "应用未启用或未安装到企业";
        case 99991672:
            return "应用缺少权限" + (result.msg.empty() ? std::string{} : ":" + result.msg) +
                   "(添加权限后需重新发布版本)";
        case 230002: return "机器人不在该群中";
        case 230006:
        case 234007: return "应用未开启机器人能力,请添加机器人能力并发布版本";
        case 230011:
        case 230110: return "消息已撤回或已删除";
        case 231003: return "消息不存在";
        case 230013: return "对方不在应用的可用范围内,请在开发者后台调整可用范围并发布版本";
        case 230017: return "无权使用该文件(不是本机器人上传的)";
        case 230018: return "该群禁止发言";
        case 230022:
        case 230028: return "消息内容未通过飞书的内容审核";
        case 230025: return "消息内容过长";
        case 230027:
        case 230035: return "没有在该会话中发消息的权限";
        case 230053: return "对方已停止接收机器人消息";
        case 230055: return "文件类型与消息类型不匹配";
        case 230099: return "卡片内容无效";
        case 232009: return "该群已解散";
        case 234003: return "附件与消息不匹配";
        case 234004: return "机器人不在该会话中";
        case 234006:
        case 234037: return "文件超过飞书的大小限制";
        case 234009: return "不支持外部群的附件";
        case 234010: return "文件为空";
        case 234011: return "不支持的文件格式";
        case 234038: return "保密会话中的附件无法下载";
        case 234039: return "图片分辨率过高";
        case 234040: return "机器人无权查看该消息";
        case 234043: return "合并转发或卡片里的附件无法下载";
        default: break;
    }
    if (result.code != 0)
        return "飞书返回错误:" + (result.msg.empty() ? std::string("未知原因") : result.msg) + code_text(result.code);
    return "飞书返回 HTTP " + std::to_string(result.status);
}

std::string describe_token_error(std::int64_t code, const std::string& msg) {
    switch (code) {
        case 10014: return "App ID 不存在或应用已停用(也请确认选择的是飞书还是 Lark)";
        case 10015: return "App Secret 不正确";
        case 10003: return "App ID 或 App Secret 格式不对";
        case 99991543: return "App ID 或 App Secret 不存在";
        default: break;
    }
    return "凭据无效:" + (msg.empty() ? std::string("未知原因") : msg) + code_text(code);
}

BotInfo parse_bot_info(const nlohmann::json& body) {
    BotInfo info;
    if (!body.is_object() || int_field(body, "code", 0) != 0) return info;
    const nlohmann::json* bot = nullptr;
    if (body.contains("bot") && body["bot"].is_object()) bot = &body["bot"];
    else if (object_field(body, "data").contains("bot") && body["data"]["bot"].is_object()) bot = &body["data"]["bot"];
    if (!bot) return info;
    info.open_id = string_field(*bot, "open_id");
    info.name = string_field(*bot, "app_name");
    info.activate_status = static_cast<int>(int_field(*bot, "activate_status", -1));
    info.ok = !info.open_id.empty();
    return info;
}

std::string describe_activate_status(int activate_status) {
    switch (activate_status) {
        case 2: return {};
        case 0: return "机器人尚未安装到企业,请发布应用版本并等待管理员审核";
        case 1: return "机器人已被企业管理员停用";
        case 3:
        case 4: return "应用已安装但尚未启用,请在开发者后台发布版本";
        case 5:
        case 6: return "应用授权已过期";
        default: break;
    }
    return "未取到机器人信息:请在开发者后台添加机器人能力并发布版本";
}

std::string make_resource_ref(const std::string& message_id, const std::string& key, const std::string& type) {
    return dump_json(nlohmann::json{{"message_id", message_id}, {"key", key}, {"type", type}});
}

std::optional<ResourceRef> parse_resource_ref(const std::string& remote_ref) {
    try {
        const auto json = nlohmann::json::parse(remote_ref);
        ResourceRef ref;
        ref.message_id = string_field(json, "message_id");
        ref.key = string_field(json, "key");
        ref.type = string_field(json, "type");
        if (ref.message_id.empty() || ref.key.empty() || (ref.type != "image" && ref.type != "file"))
            return std::nullopt;
        return ref;
    } catch (...) {
        return std::nullopt;
    }
}

std::string dump_json(const nlohmann::json& value) {
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

SendTarget send_target(const Address& to, const nlohmann::json& reply_context) {
    SendTarget target;
    std::string chat_id;
    if (reply_context.is_object()) {
        target.reply_to = string_field(reply_context, "message_id");
        chat_id = string_field(reply_context, "chat_id");
        target.in_thread = !string_field(reply_context, "thread_id").empty();
    }
    if (!chat_id.empty()) {
        target.receive_id_type = "chat_id";
        target.receive_id = chat_id;
    } else if (to.kind == ChatKind::Group) {
        target.receive_id_type = "chat_id";
        target.receive_id = to.chat;
    } else {
        target.receive_id_type = "open_id";
        target.receive_id = to.chat;
    }
    if (target.reply_to.empty()) target.in_thread = false;
    return target;
}

FileRoute route_file(const std::string& name, const std::string& mime_type, std::uint64_t size) {
    const auto file = lower(name);
    const auto mime = lower(mime_type);
    const auto has_ext = [&file](std::initializer_list<const char*> exts) {
        return std::any_of(exts.begin(), exts.end(), [&file](const char* ext) { return ends_with(file, ext); });
    };
    FileRoute route;
    const bool image_ext = has_ext({".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp", ".ico", ".tif", ".tiff", ".heic"});
    const bool image_mime = starts_with(mime, "image/") && mime != "image/svg+xml";
    if ((image_ext || image_mime) && size > 0 && size <= kMaxImageBytes) {
        route.image = true;
        route.msg_type = "image";
        return route;
    }
    route.msg_type = "file";
    if (has_ext({".opus"}) || mime == "audio/opus") {
        route.file_type = "opus";
        route.msg_type = "audio";
    } else if (has_ext({".mp4"}) || mime == "video/mp4") {
        route.file_type = "mp4";
        route.msg_type = "media";
    } else if (has_ext({".pdf"})) {
        route.file_type = "pdf";
    } else if (has_ext({".doc", ".docx"})) {
        route.file_type = "doc";
    } else if (has_ext({".xls", ".xlsx"})) {
        route.file_type = "xls";
    } else if (has_ext({".ppt", ".pptx"})) {
        route.file_type = "ppt";
    } else {
        route.file_type = "stream";
    }
    return route;
}

} // namespace acecode::im::feishu
