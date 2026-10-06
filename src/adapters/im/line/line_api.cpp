#include "im/line/line_api.hpp"

#include "im/line/line_protocol.hpp"
#include "im/redact.hpp"
#include "platform/crypto/secure_random.hpp"

#include <algorithm>
#include <thread>

namespace acecode::im::line {
namespace {

using Clock = std::chrono::steady_clock;

std::string trim_slash(std::string base) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base;
}

std::string json_str(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return {};
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

// 可取消的等待;返回 false 表示被取消。
bool wait_cancellable(std::chrono::milliseconds duration, const CancelFn& cancel) {
    const auto deadline = Clock::now() + duration;
    while (Clock::now() < deadline) {
        if (cancel && cancel()) return false;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        std::this_thread::sleep_for((std::min)(left, std::chrono::milliseconds(50)));
    }
    return !(cancel && cancel());
}

constexpr const char* kInvalidIdSecret = "Channel ID 或 Channel secret 无效,请在 LINE Developers Console 的 Basic settings 页核对";
constexpr const char* kInvalidLongLived = "Channel access token 无效或已失效,请重新签发,或改用 Channel ID + Channel secret";

} // namespace

Api::Api(ApiOptions options) : options_(std::move(options)) {
    options_.api_base = trim_slash(options_.api_base);
    options_.data_api_base = trim_slash(options_.data_api_base);
}

std::vector<std::string> Api::secrets() const {
    std::vector<std::string> out{options_.channel_secret, options_.access_token};
    std::lock_guard<std::mutex> lock(mu_);
    if (!token_.empty()) out.push_back(token_);
    return out;
}

std::string Api::mint_locked(std::string* error, bool* auth_failed, const CancelFn& cancel) {
    HttpRequest request;
    request.method = "POST";
    request.url = options_.api_base + "/oauth2/v3/token";
    request.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
    request.body = "grant_type=client_credentials&client_id=" + percent_encode(options_.channel_id) +
                   "&client_secret=" + percent_encode(options_.channel_secret);
    request.timeout = options_.timeout;
    request.use_proxy = options_.use_proxy;
    if (cancel) request.cancel = cancel;
    const auto response = http_send(request);
    const std::vector<std::string> secrets{options_.channel_secret, options_.access_token};
    if (response.status == 0) {
        if (error) {
            *error = redact_secrets(response.error.empty() ? std::string("无法连接 LINE")
                                                           : "无法连接 LINE:" + response.error,
                                    secrets);
        }
        return {};
    }
    if (response.status == 400 || response.status == 401) {
        // 换取令牌被拒只可能是 Channel ID / secret 不对(或频道被删除),不会自己好。
        if (auth_failed) *auth_failed = true;
        if (error) *error = kInvalidIdSecret;
        return {};
    }
    if (response.status < 200 || response.status >= 300) {
        if (error) *error = "LINE 令牌服务暂时不可用(HTTP " + std::to_string(response.status) + ")";
        return {};
    }
    try {
        const auto json = nlohmann::json::parse(response.body);
        const auto token = json_str(json, "access_token");
        if (!token.empty()) {
            long long seconds = 900;
            if (json.contains("expires_in") && json["expires_in"].is_number_integer())
                seconds = json["expires_in"].get<long long>();
            // 到期前 60 秒重换;有效期很短时至少留一半。
            const long long margin = (std::min)(60LL, (std::max)(seconds / 2, 1LL));
            token_ = token;
            refresh_at_ = Clock::now() + std::chrono::seconds((std::max)(seconds - margin, 1LL));
            return token_;
        }
    } catch (...) {
    }
    if (error) *error = "LINE 令牌服务返回了无效数据";
    return {};
}

std::string Api::access_token(std::string* error, bool* auth_failed, const CancelFn& cancel) {
    if (auth_failed) *auth_failed = false;
    if (!options_.access_token.empty()) return options_.access_token;
    std::lock_guard<std::mutex> lock(mu_);
    if (!token_.empty() && Clock::now() < refresh_at_) return token_;
    token_.clear();
    if (options_.channel_id.empty() || options_.channel_secret.empty()) {
        if (auth_failed) *auth_failed = true;
        if (error) *error = "缺少 Channel ID 或 Channel secret";
        return {};
    }
    return mint_locked(error, auth_failed, cancel);
}

void Api::invalidate_token() {
    std::lock_guard<std::mutex> lock(mu_);
    token_.clear();
    refresh_at_ = {};
}

ApiResult Api::parse(const HttpResponse& response) const {
    ApiResult result;
    result.status = response.status;
    result.cancelled = response.cancelled;
    const auto hidden = secrets();
    if (response.status == 0) {
        result.message = redact_secrets(response.error.empty() ? std::string("无法连接 LINE")
                                                               : "无法连接 LINE:" + response.error,
                                        hidden);
        return result;
    }
    result.ok = response.status >= 200 && response.status < 300;
    if (!response.body.empty()) {
        try {
            auto json = nlohmann::json::parse(response.body);
            if (json.is_object()) result.body = std::move(json);
        } catch (...) {
        }
    }
    std::string message = json_str(result.body, "message");
    if (message.empty()) message = json_str(result.body, "error_description");
    if (message.empty()) message = json_str(result.body, "error");
    if (result.body.is_object() && result.body.contains("details") && result.body["details"].is_array() &&
        !result.body["details"].empty()) {
        const auto& detail = result.body["details"].front();
        const auto text = json_str(detail, "message");
        const auto property = json_str(detail, "property");
        if (!text.empty()) message += " (" + (property.empty() ? text : property + ": " + text) + ")";
    }
    if (!result.ok && message.empty()) message = "LINE 请求失败(HTTP " + std::to_string(response.status) + ")";
    result.message = redact_secrets(message, hidden);
    return result;
}

ApiResult Api::call(const std::string& method, const std::string& base, const std::string& path,
                    const std::string& body, const std::vector<std::pair<std::string, std::string>>& headers,
                    const CancelFn& cancel) {
    ApiResult result;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string error;
        bool auth_failed = false;
        const auto token = access_token(&error, &auth_failed, cancel);
        if (token.empty()) {
            result = ApiResult{};
            result.auth_failed = auth_failed;
            result.message = error;
            result.cancelled = cancel && cancel();
            return result;
        }
        HttpRequest request;
        request.method = method;
        request.url = base + path;
        request.headers = {{"Authorization", "Bearer " + token}};
        if (method != "GET") request.headers.emplace_back("Content-Type", "application/json");
        for (const auto& header : headers) request.headers.push_back(header);
        request.body = body;
        request.timeout = options_.timeout;
        request.use_proxy = options_.use_proxy;
        if (cancel) request.cancel = cancel;
        result = parse(http_send(request));
        if (result.status == 401) {
            // 无状态令牌可能刚好过期或被平台提前作废:重换一次再试;长期令牌 401 就是失效了。
            if (!uses_long_lived_token() && attempt == 0) {
                invalidate_token();
                continue;
            }
            result.auth_failed = true;
            result.message = uses_long_lived_token() ? kInvalidLongLived : kInvalidIdSecret;
        }
        return result;
    }
    return result;
}

ApiResult Api::get(const std::string& path, const CancelFn& cancel) {
    return call("GET", options_.api_base, path, {}, {}, cancel);
}

// 模型输出偶尔夹带非法 UTF-8;dump 默认会抛异常,这里替换成 U+FFFD,不让一次发送把线程打崩。
static std::string dump_json(const nlohmann::json& body) {
    return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

ApiResult Api::post(const std::string& path, const nlohmann::json& body,
                    const std::vector<std::pair<std::string, std::string>>& headers, const CancelFn& cancel) {
    return call("POST", options_.api_base, path, dump_json(body), headers, cancel);
}

ApiResult Api::put(const std::string& path, const nlohmann::json& body, const CancelFn& cancel) {
    return call("PUT", options_.api_base, path, dump_json(body), {}, cancel);
}

ApiResult Api::bot_info(BotInfo* info, const CancelFn& cancel) {
    auto result = get("/v2/bot/info", cancel);
    if (result.ok && info) {
        info->user_id = json_str(result.body, "userId");
        info->basic_id = json_str(result.body, "basicId");
        info->display_name = json_str(result.body, "displayName");
        info->picture_url = json_str(result.body, "pictureUrl");
        info->chat_mode = json_str(result.body, "chatMode");
    }
    return result;
}

ApiResult Api::webhook_info(WebhookInfo* info, const CancelFn& cancel) {
    auto result = get("/v2/bot/channel/webhook/endpoint", cancel);
    if (info) {
        *info = WebhookInfo{};
        if (result.ok) {
            info->exists = true;
            info->endpoint = json_str(result.body, "endpoint");
            info->active = json_bool(result.body, "active");
        }
    }
    return result;
}

ApiResult Api::set_webhook(const std::string& endpoint, const CancelFn& cancel) {
    return put("/v2/bot/channel/webhook/endpoint", {{"endpoint", endpoint}}, cancel);
}

ApiResult Api::test_webhook(const std::string& endpoint, const CancelFn& cancel) {
    return post("/v2/bot/channel/webhook/test", {{"endpoint", endpoint}}, {}, cancel);
}

ApiResult Api::reply(const std::string& reply_token, const nlohmann::json& messages) {
    // 回复不能带 X-Line-Retry-Key(会被 400 拒绝)。
    return post("/v2/bot/message/reply", {{"replyToken", reply_token}, {"messages", messages}});
}

ApiResult Api::push(const std::string& to, const nlohmann::json& messages, const std::string& retry_key) {
    std::vector<std::pair<std::string, std::string>> headers;
    if (!retry_key.empty()) headers.emplace_back("X-Line-Retry-Key", retry_key);
    return post("/v2/bot/message/push", {{"to", to}, {"messages", messages}}, headers);
}

ApiResult Api::start_loading(const std::string& chat_id, int seconds) {
    // 只接受 5 的倍数,范围 5..60。
    seconds = (std::max)(5, (std::min)(60, seconds));
    seconds -= seconds % 5;
    return post("/v2/bot/chat/loading/start", {{"chatId", chat_id}, {"loadingSeconds", seconds}});
}

ApiResult Api::display_name(const std::string& chat_id, const std::string& user_id, std::string* name) {
    std::string path;
    if (!chat_id.empty() && chat_id[0] == 'C')
        path = "/v2/bot/group/" + percent_encode(chat_id) + "/member/" + percent_encode(user_id);
    else if (!chat_id.empty() && chat_id[0] == 'R')
        path = "/v2/bot/room/" + percent_encode(chat_id) + "/member/" + percent_encode(user_id);
    else
        path = "/v2/bot/profile/" + percent_encode(user_id);
    auto result = get(path);
    if (result.ok && name) *name = json_str(result.body, "displayName");
    return result;
}

ApiResult Api::quota(nlohmann::json* summary, const CancelFn& cancel) {
    auto limit = get("/v2/bot/message/quota", cancel);
    if (!limit.ok) return limit;
    auto usage = get("/v2/bot/message/quota/consumption", cancel);
    if (summary) {
        *summary = nlohmann::json::object();
        (*summary)["type"] = json_str(limit.body, "type");
        if (limit.body.is_object() && limit.body.contains("value")) (*summary)["value"] = limit.body["value"];
        if (usage.ok && usage.body.is_object() && usage.body.contains("totalUsage"))
            (*summary)["used"] = usage.body["totalUsage"];
    }
    return usage.ok ? limit : usage;
}

bool Api::fetch_content(const std::string& url, const std::filesystem::path& dest, std::uint64_t max_bytes,
                        bool with_auth, HttpResponse* out, const CancelFn& cancel) {
    HttpRequest request;
    request.url = url;
    if (with_auth) {
        std::string error;
        bool auth_failed = false;
        const auto token = access_token(&error, &auth_failed, cancel);
        if (token.empty()) {
            out->status = 0;
            out->error = error;
            return false;
        }
        request.headers = {{"Authorization", "Bearer " + token}};
    }
    request.timeout = std::chrono::minutes(2);
    request.use_proxy = options_.use_proxy;
    request.download_to = dest;
    request.max_download_bytes = max_bytes;
    if (cancel) request.cancel = cancel;
    *out = http_send(request);
    return out->status >= 200 && out->status < 300 && out->status != 202 && !out->too_large;
}

bool Api::download_content(const std::string& message_id, const std::filesystem::path& dest, std::uint64_t max_bytes,
                           std::string* error, const CancelFn& cancel) {
    const auto content = "/v2/bot/message/" + percent_encode(message_id) + "/content";
    bool refreshed = false;
    int waits = 0;
    while (true) {
        HttpResponse response;
        if (fetch_content(options_.data_api_base + content, dest, max_bytes, true, &response, cancel)) return true;
        std::error_code ec;
        std::filesystem::remove(dest, ec);
        if (response.too_large) {
            if (error) *error = "附件超过大小限制";
            return false;
        }
        if (response.status == 401 && !refreshed && !uses_long_lived_token()) {
            refreshed = true;
            invalidate_token();
            continue;
        }
        if (response.status == 202) {
            // 视频 / 音频转码中:按 /content/transcoding 的状态等待。
            if (++waits > options_.transcoding_attempts) {
                if (error) *error = "LINE 仍在处理该附件,请稍后重发";
                return false;
            }
            const auto state = call("GET", options_.data_api_base, content + "/transcoding", {}, {}, cancel);
            const auto status = json_str(state.body, "status");
            if (status == "failed") {
                if (error) *error = "LINE 无法处理该附件";
                return false;
            }
            if (status != "succeeded" && !wait_cancellable(options_.transcoding_poll, cancel)) {
                if (error) *error = "下载已取消";
                return false;
            }
            continue;
        }
        if (error) {
            if (response.status == 404) *error = "附件不存在或已过期";
            else if (response.status == 410) *error = "对方已撤回该消息";
            else if (response.status == 401) *error = uses_long_lived_token() ? kInvalidLongLived : kInvalidIdSecret;
            else if (response.status == 0)
                *error = redact_secrets("下载附件失败:" + (response.error.empty() ? std::string("无法连接 LINE")
                                                                                : response.error),
                                        secrets());
            else *error = "下载附件失败:HTTP " + std::to_string(response.status);
        }
        return false;
    }
}

bool Api::download_url(const std::string& url, const std::filesystem::path& dest, std::uint64_t max_bytes,
                       std::string* error, const CancelFn& cancel) {
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
        if (error) *error = "附件地址无效";
        return false;
    }
    HttpResponse response;
    if (fetch_content(url, dest, max_bytes, false, &response, cancel)) return true;
    std::error_code ec;
    std::filesystem::remove(dest, ec);
    if (error) {
        if (response.too_large) *error = "附件超过大小限制";
        else if (response.status == 0) *error = redact_secrets("下载附件失败:" + response.error, secrets());
        else *error = "下载附件失败:HTTP " + std::to_string(response.status);
    }
    return false;
}

std::string new_retry_key() {
    auto bytes = platform::secure_random_bytes(16);
    if (bytes.size() != 16) return {};
    bytes[6] = static_cast<char>((static_cast<unsigned char>(bytes[6]) & 0x0Fu) | 0x40u);
    bytes[8] = static_cast<char>((static_cast<unsigned char>(bytes[8]) & 0x3Fu) | 0x80u);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        const auto u = static_cast<unsigned char>(bytes[i]);
        out.push_back(kHex[u >> 4]);
        out.push_back(kHex[u & 0x0F]);
    }
    return out;
}

Validation validate_credentials(const ApiOptions& options) {
    Validation validation;
    if (options.channel_secret.empty()) {
        validation.error = "请填写 Channel secret(LINE Developers Console 的 Basic settings 页)";
        return validation;
    }
    if (options.channel_id.empty() && options.access_token.empty()) {
        validation.error = "请填写 Channel ID(LINE Developers Console 的 Basic settings 页)";
        return validation;
    }
    if (!options.channel_id.empty() && !options.access_token.empty()) {
        // 同时给了长期令牌:仍用 Channel ID + secret 换一次令牌,确认 secret 正确(回调签名要用它)。
        auto minting = options;
        minting.access_token.clear();
        Api minter(minting);
        std::string error;
        bool auth_failed = false;
        if (minter.access_token(&error, &auth_failed).empty()) {
            validation.network_error = !auth_failed;
            validation.error = auth_failed ? std::string(kInvalidIdSecret) : error;
            return validation;
        }
    }
    Api api(options);
    BotInfo bot;
    const auto result = api.bot_info(&bot);
    if (!result.ok) {
        if (result.auth_failed) {
            validation.error = api.uses_long_lived_token() ? kInvalidLongLived : kInvalidIdSecret;
        } else if (result.status == 0) {
            validation.network_error = true;
            validation.error = result.message.empty() ? std::string("无法连接 LINE") : result.message;
        } else {
            validation.error = "LINE 校验失败:" + result.message;
        }
        return validation;
    }
    if (bot.user_id.empty()) {
        validation.error = "LINE 返回的机器人信息无效";
        return validation;
    }
    validation.ok = true;
    validation.bot = bot;
    return validation;
}

} // namespace acecode::im::line
