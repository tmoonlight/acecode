#include "dingtalk_api.hpp"

#include "im/http.hpp"
#include "im/redact.hpp"
#include "utils/logger.hpp"

#include <algorithm>

namespace acecode::im::dingtalk {
namespace {

constexpr std::chrono::seconds kTokenRefreshMargin{300};

std::string trim_slash(std::string base) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base;
}

nlohmann::json parse_object(const std::string& body) {
    try {
        auto json = nlohmann::json::parse(body);
        if (json.is_object()) return json;
    } catch (...) {
    }
    return nlohmann::json::object();
}

std::string json_text(const nlohmann::json& value, const char* key) {
    if (!value.is_object() || !value.contains(key)) return {};
    const auto& field = value.at(key);
    if (field.is_string()) return field.get<std::string>();
    if (field.is_number_integer()) return std::to_string(field.get<std::int64_t>());
    return {};
}

long long expire_seconds(const nlohmann::json& value, const char* key) {
    if (!value.is_object() || !value.contains(key)) return 7200;
    const auto& field = value.at(key);
    if (field.is_number_integer()) return field.get<long long>();
    if (field.is_string()) {
        try {
            return std::stoll(field.get<std::string>());
        } catch (...) {
        }
    }
    return 7200;
}

bool list_contains(const nlohmann::json& response, const char* key, const std::string& id) {
    if (!response.contains(key) || !response[key].is_array()) return false;
    for (const auto& item : response[key]) {
        if (item.is_string() && item.get<std::string>() == id) return true;
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> json_headers() {
    return {{"Content-Type", "application/json"}, {"Accept", "application/json"}};
}

std::string log_detail(const ApiError& error) {
    std::string text = "HTTP " + std::to_string(error.status);
    if (!error.code.empty()) text += " code=" + error.code;
    if (error.errcode != 0) text += " errcode=" + std::to_string(error.errcode);
    if (!error.request_id.empty()) text += " requestid=" + error.request_id;
    return text;
}

// Stream 注册的 ua 字段,格式沿用官方 SDK 的 name-sdk-lang/version。
std::string stream_ua() {
    const auto agent = user_agent();
    const auto slash = agent.find('/');
    return "acecode-stream-cpp/" + (slash == std::string::npos ? agent : agent.substr(slash + 1));
}

} // namespace

Api::Api(ApiOptions options) : options_(std::move(options)) {}

std::string Api::robot_code() const { return options_.robot_code.empty() ? options_.client_id : options_.robot_code; }

std::vector<std::string> Api::secrets() const {
    std::lock_guard<std::mutex> lock(mu_);
    return {options_.client_secret, token_};
}

std::vector<std::string> Api::webhook_bases() const { return {options_.api_base, options_.oapi_base}; }

std::string Api::fetch_token_locked(std::string* error) {
    HttpRequest request;
    request.method = "POST";
    request.url = trim_slash(options_.api_base) + "/v1.0/oauth2/accessToken";
    request.headers = json_headers();
    request.body = nlohmann::json{{"appKey", options_.client_id}, {"appSecret", options_.client_secret}}.dump();
    request.timeout = std::chrono::seconds(15);
    request.use_proxy = options_.use_proxy;
    const auto response = http_send(request);
    const std::vector<std::string> secrets{options_.client_secret};
    auth_failed_ = false;
    last_token_status_ = response.status;
    token_.clear();
    if (response.status == 0) {
        if (error) *error = redact_secrets(response.error.empty() ? "无法连接钉钉开放平台" : response.error, secrets);
        return {};
    }
    const auto json = parse_object(response.body);
    const auto token = json_text(json, "accessToken");
    if (response.status >= 200 && response.status < 300 && !token.empty()) {
        token_ = token;
        const auto seconds = expire_seconds(json, "expireIn");
        const auto margin = std::min<long long>(kTokenRefreshMargin.count(), std::max<long long>(seconds / 2, 1));
        refresh_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(std::max<long long>(seconds - margin, 1));
        return token_;
    }
    const auto api_error = parse_api_error(response.status, response.body);
    // 换令牌接口的 4xx 几乎都是凭据问题(invalidClientIdOrSecret / MissingappKey);429 与 5xx 是临时问题。
    auth_failed_ = response.status >= 400 && response.status < 500 && response.status != 429;
    LOG_WARN("[channels/dingtalk] access token request failed: " + log_detail(api_error));
    if (error) {
        *error = auth_failed_ ? std::string("Client ID 或 Client Secret 不正确")
                              : redact_secrets("获取钉钉访问令牌失败:" + api_error.message, secrets);
    }
    return {};
}

std::string Api::access_token(std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!token_.empty() && std::chrono::steady_clock::now() < refresh_at_) return token_;
    return fetch_token_locked(error);
}

void Api::invalidate_token() {
    std::lock_guard<std::mutex> lock(mu_);
    token_.clear();
    refresh_at_ = {};
}

bool Api::last_token_auth_failed() const {
    std::lock_guard<std::mutex> lock(mu_);
    return auth_failed_;
}

StreamTicket Api::open_stream(std::chrono::milliseconds timeout) {
    StreamTicket ticket;
    HttpRequest request;
    request.method = "POST";
    request.url = trim_slash(options_.api_base) + "/v1.0/gateway/connections/open";
    request.headers = json_headers();  // 不带 Accept 时平台默认回 XML
    request.body = nlohmann::json{{"clientId", options_.client_id},
                                  {"clientSecret", options_.client_secret},
                                  {"subscriptions", nlohmann::json::array({{{"type", "CALLBACK"},
                                                                            {"topic", kBotMessageTopic}}})},
                                  {"ua", stream_ua()}}
                       .dump();
    request.timeout = timeout;
    request.use_proxy = options_.use_proxy;
    const auto response = http_send(request);
    const std::vector<std::string> secrets{options_.client_secret};
    if (response.status == 0) {
        ticket.error.message = redact_secrets(response.error, secrets);
        ticket.reason = describe_api_error(ticket.error);
        return ticket;
    }
    if (response.status >= 200 && response.status < 300) {
        const auto json = parse_object(response.body);
        ticket.endpoint = json_text(json, "endpoint");
        ticket.ticket = json_text(json, "ticket");
        if (!ticket.endpoint.empty() && !ticket.ticket.empty()) {
            ticket.ok = true;
            return ticket;
        }
        ticket.error.status = response.status;
        ticket.error.message = "missing endpoint or ticket";
        ticket.reason = "钉钉 Stream 注册返回了无效的接入地址";
        LOG_WARN("[channels/dingtalk] connections/open returned no endpoint/ticket");
        return ticket;
    }
    ticket.error = parse_api_error(response.status, response.body);
    ticket.error.message = redact_secrets(ticket.error.message, secrets);
    LOG_WARN("[channels/dingtalk] connections/open failed: " + log_detail(ticket.error));
    if (response.status == 401 || ticket.error.code == "authFailed" ||
        ticket.error.code == "invalidClientIdOrSecret") {
        ticket.auth_failed = true;
        ticket.reason = "Client ID 或 Client Secret 无效,请在钉钉开发者后台核对后重新连接";
    } else if (response.status >= 400 && response.status < 500 && response.status != 429) {
        ticket.rejected = true;
        ticket.reason = "钉钉拒绝建立 Stream 连接:请确认应用已发布、机器人已开启 Stream 模式,并检查服务器出口 IP 白名单";
    } else {
        ticket.reason = "钉钉 Stream 注册暂时失败(HTTP " + std::to_string(response.status) + "),稍后自动重试";
    }
    return ticket;
}

VerifyResult Api::verify() {
    VerifyResult result;
    result.client_id = options_.client_id;
    result.robot_code = robot_code();
    if (options_.client_id.empty() || options_.client_secret.empty()) {
        result.auth_failed = true;
        result.error = "请填写 Client ID 和 Client Secret";
        return result;
    }
    std::string error;
    std::string token;
    bool auth_failed = false;
    long status = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        token_.clear();
        token = fetch_token_locked(&error);
        auth_failed = auth_failed_;
        status = last_token_status_;
    }
    if (token.empty()) {
        if (auth_failed) {
            result.auth_failed = true;
            result.error = "Client ID 或 Client Secret 不正确,请在钉钉开发者后台「凭证与基础信息」中核对";
        } else if (status == 0) {
            result.network = true;
            result.error = "无法连接钉钉开放平台,请检查网络或代理设置";
        } else {
            result.error = error.empty() ? std::string("获取钉钉访问令牌失败") : error;
        }
        return result;
    }
    const auto ticket = open_stream(std::chrono::seconds(10));
    if (ticket.ok) {
        result.ok = true;
        return result;
    }
    if (ticket.auth_failed) {
        result.auth_failed = true;
        result.error = "Client ID 或 Client Secret 不正确,请在钉钉开发者后台「凭证与基础信息」中核对";
    } else if (ticket.rejected) {
        result.rejected = true;
        result.error = ticket.reason;
    } else if (ticket.error.status == 0) {
        result.network = true;
        result.error = "无法连接钉钉开放平台,请检查网络或代理设置";
    } else {
        result.error = ticket.reason;
    }
    return result;
}

ApiResult Api::post_api(const std::string& path, const nlohmann::json& body, std::chrono::milliseconds timeout) {
    ApiResult result;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string error;
        const auto token = access_token(&error);
        if (token.empty()) {
            result.error.message = error;
            if (last_token_auth_failed()) {
                result.error.status = 401;
                result.error.code = "invalidClientIdOrSecret";
            }
            return result;
        }
        HttpRequest request;
        request.method = "POST";
        request.url = trim_slash(options_.api_base) + path;
        request.headers = json_headers();
        request.headers.emplace_back("x-acs-dingtalk-access-token", token);
        request.body = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        request.timeout = timeout;
        request.use_proxy = options_.use_proxy;
        const auto response = http_send(request);
        const std::vector<std::string> secrets{options_.client_secret, token};
        if (response.status == 0) {
            result.error = ApiError{};
            result.error.message = redact_secrets(response.error, secrets);
            return result;
        }
        ApiError error_body;
        if (response.status >= 200 && response.status < 300 && !body_has_errcode(response.body, &error_body)) {
            result.ok = true;
            result.response = parse_object(response.body);
            return result;
        }
        result.error = response.status >= 200 && response.status < 300 ? error_body
                                                                          : parse_api_error(response.status, response.body);
        result.error.message = redact_secrets(result.error.message, secrets);
        if (is_token_error(result.error) && attempt == 0) {
            invalidate_token();
            continue;
        }
        LOG_WARN("[channels/dingtalk] " + path + " failed: " + log_detail(result.error));
        return result;
    }
    return result;
}

ApiResult Api::send_webhook(const std::string& url, const nlohmann::json& body) {
    ApiResult result;
    if (!webhook_url_allowed(url, webhook_bases())) {
        result.error.status = 400;
        result.error.message = "会话回复地址不是钉钉域名,已拒绝";
        LOG_WARN("[channels/dingtalk] refused a session webhook outside the DingTalk domains");
        return result;
    }
    HttpRequest request;
    request.method = "POST";
    request.url = url;
    request.headers = json_headers();
    request.body = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    request.timeout = std::chrono::seconds(30);
    request.use_proxy = options_.use_proxy;
    const auto response = http_send(request);
    // webhook 的 session 参数相当于 90 分钟内的回复凭据,不能出现在任何错误文本里。
    const std::vector<std::string> secrets{options_.client_secret, url};
    if (response.status == 0) {
        result.error.message = redact_secrets(response.error, secrets);
        return result;
    }
    ApiError error;
    if (response.status >= 200 && response.status < 300) {
        if (!body_has_errcode(response.body, &error)) {
            result.ok = true;
            result.response = parse_object(response.body);
            return result;
        }
    } else {
        error = parse_api_error(response.status, response.body);
    }
    error.message = redact_secrets(error.message, secrets);
    result.error = error;
    LOG_WARN("[channels/dingtalk] session webhook send failed: " + log_detail(error));
    return result;
}

ApiResult Api::send_oto(const std::string& robot_code, const std::string& staff_id, const std::string& msg_key,
                        const nlohmann::json& msg_param) {
    const nlohmann::json body{{"robotCode", robot_code},
                              {"userIds", nlohmann::json::array({staff_id})},
                              {"msgKey", msg_key},
                              {"msgParam", msg_param.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)}};
    auto result = post_api("/v1.0/robot/oToMessages/batchSend", body, std::chrono::seconds(30));
    if (!result.ok) return result;
    const auto& response = result.response;
    result.ok = false;
    result.error.status = 200;
    if (list_contains(response, "flowControlledStaffIdList", staff_id)) {
        result.error.code = "send.too.fast";
        result.error.message = "发送频率过快";
    } else if (list_contains(response, "invalidStaffIdList", staff_id)) {
        result.error.code = "invalidStaffId";
        result.error.message = "对方不在机器人的可见范围内,或不是本组织成员";
    } else if (list_contains(response, "filteredStaffIdList", staff_id)) {
        result.error.code = "filteredStaffId";
        result.error.message = "钉钉过滤了这条消息(对方可能关闭了机器人消息)";
    } else if (json_text(response, "processQueryKey").empty()) {
        result.error.message = "钉钉没有确认这条消息";
    } else {
        result.ok = true;
        result.error = ApiError{};
        return result;
    }
    LOG_WARN("[channels/dingtalk] oToMessages/batchSend not delivered: " + result.error.code);
    return result;
}

ApiResult Api::send_group(const std::string& robot_code, const std::string& conversation_id,
                          const std::string& msg_key, const nlohmann::json& msg_param) {
    const nlohmann::json body{{"robotCode", robot_code},
                              {"openConversationId", conversation_id},
                              {"msgKey", msg_key},
                              {"msgParam", msg_param.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)}};
    auto result = post_api("/v1.0/robot/groupMessages/send", body, std::chrono::seconds(30));
    if (result.ok && json_text(result.response, "processQueryKey").empty()) {
        result.ok = false;
        result.error.status = 200;
        result.error.message = "钉钉没有确认这条消息";
    }
    return result;
}

ApiResult Api::upload_media(const std::string& type, const std::filesystem::path& path, const std::string& name,
                            const std::string& mime_type) {
    ApiResult result;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string error;
        const auto token = access_token(&error);
        if (token.empty()) {
            result.error.message = error;
            return result;
        }
        HttpRequest request;
        request.method = "POST";
        // type 同时放查询参数与表单字段(python SDK 用表单字段,官方 connector 用查询参数)。
        request.url = trim_slash(options_.oapi_base) + "/media/upload?access_token=" + url_encode(token) +
                      "&type=" + url_encode(type);
        HttpPart type_part;
        type_part.name = "type";
        type_part.value = type;
        HttpPart media;
        media.name = "media";
        media.file = path;
        media.filename = name;
        media.content_type = mime_type.empty() ? std::string("application/octet-stream") : mime_type;
        request.parts = {type_part, media};
        request.timeout = std::chrono::seconds(120);
        request.use_proxy = options_.use_proxy;
        const auto response = http_send(request);
        const std::vector<std::string> secrets{options_.client_secret, token};
        if (response.status == 0) {
            result.error = ApiError{};
            result.error.message = redact_secrets(response.error, secrets);
            return result;
        }
        ApiError api_error;
        const bool http_ok = response.status >= 200 && response.status < 300;
        if (http_ok && !body_has_errcode(response.body, &api_error)) {
            result.response = parse_object(response.body);
            if (!json_text(result.response, "media_id").empty()) {
                result.ok = true;
                result.error = ApiError{};
                return result;
            }
            result.error = ApiError{};
            result.error.status = response.status;
            result.error.message = "钉钉上传结果缺少 media_id";
            return result;
        }
        result.error = http_ok ? api_error : parse_api_error(response.status, response.body);
        result.error.message = redact_secrets(result.error.message, secrets);
        if (is_token_error(result.error) && attempt == 0) {
            invalidate_token();
            continue;
        }
        LOG_WARN("[channels/dingtalk] media/upload failed: " + log_detail(result.error));
        return result;
    }
    return result;
}

std::string Api::download_url(const std::string& download_code, const std::string& robot_code, std::string* error) {
    const auto result = post_api("/v1.0/robot/messageFiles/download",
                                 {{"downloadCode", download_code}, {"robotCode", robot_code}}, std::chrono::seconds(30));
    if (!result.ok) {
        if (error) *error = "获取附件下载地址失败:" + describe_api_error(result.error);
        return {};
    }
    const auto url = json_text(result.response, "downloadUrl");
    if (url.empty() && error) *error = "钉钉没有返回附件下载地址";
    return url;
}

bool Api::download(const std::string& download_code, const std::string& robot_code, const std::filesystem::path& dest,
                   std::uint64_t max_bytes, std::string* error) {
    const auto url = download_url(download_code, robot_code, error);
    if (url.empty()) return false;
    HttpRequest request;
    request.url = url;  // 不带 Content-Type 与鉴权头:OSS 签名校验只认原始请求
    request.timeout = std::chrono::seconds(120);
    request.use_proxy = options_.use_proxy;
    request.download_to = dest;
    request.max_download_bytes = max_bytes;
    const auto response = http_send(request);
    if (response.too_large) {
        if (error) *error = "附件超过大小限制";
        return false;
    }
    if (response.status < 200 || response.status >= 300) {
        std::error_code ec;
        std::filesystem::remove(dest, ec);
        if (error) {
            *error = response.status == 0 ? "下载附件失败:" + redact_secrets(response.error, {url})
                                          : "下载附件失败:HTTP " + std::to_string(response.status);
        }
        return false;
    }
    return true;
}

} // namespace acecode::im::dingtalk
