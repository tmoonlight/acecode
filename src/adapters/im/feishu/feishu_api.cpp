#include "im/feishu/feishu_api.hpp"

#include "im/redact.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace acecode::im::feishu {
namespace {

using Clock = std::chrono::steady_clock;

std::string trim_slash(std::string base) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base;
}

std::string path_segment(const std::string& value) {
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

std::function<bool()> cancel_check(const std::atomic<bool>* cancel) {
    if (!cancel) return {};
    return [cancel] { return cancel->load(); };
}

// 下载失败时响应体(JSON 错误)已经写进了目标文件:读回一小段用于解析错误码。
std::string read_head(const std::filesystem::path& path, std::size_t limit) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string data(limit, '\0');
    in.read(data.data(), static_cast<std::streamsize>(limit));
    data.resize(static_cast<std::size_t>(in.gcount()));
    return data;
}

// 限流等待秒数与请求 id 只在响应头里(键已由 http_send 转成小写)。
void attach_response_headers(ApiResult& result, const HttpResponse& response) {
    const auto logid = response.headers.find("x-tt-logid");
    if (logid != response.headers.end()) result.log_id = logid->second;
    const auto reset = response.headers.find("x-ogw-ratelimit-reset");
    if (reset != response.headers.end()) result.retry_after = parse_retry_after(reset->second);
}

} // namespace

std::string feishu_user_agent() { return user_agent() + " source/acecode channel"; }

Api::Api(ApiOptions options) : options_(std::move(options)) {}

std::string Api::url(const std::string& path) const { return trim_slash(options_.base) + path; }

std::string Api::redact(const std::string& text) const { return redact_secrets(text, {options_.app_secret}); }

std::string Api::fetch_token_locked(std::string* error, const std::atomic<bool>* cancel) {
    HttpRequest request;
    request.method = "POST";
    request.url = url("/open-apis/auth/v3/tenant_access_token/internal");
    request.headers = {{"Content-Type", "application/json; charset=utf-8"}, {"User-Agent", feishu_user_agent()}};
    request.body = dump_json({{"app_id", options_.app_id}, {"app_secret", options_.app_secret}});
    request.timeout = std::chrono::seconds(15);
    request.use_proxy = options_.use_proxy;
    request.cancel = cancel_check(cancel);
    const auto response = http_send(request);
    auth_failed_ = false;
    if (response.cancelled) {
        if (error) *error = "已取消";
        return {};
    }
    if (response.status == 0) {
        if (error)
            *error = redact("无法连接飞书开放平台" + (response.error.empty() ? std::string{} : ":" + response.error));
        return {};
    }
    const auto result = parse_api_response(response.status, response.body);
    const auto token = result.body.is_object() && result.body.contains("tenant_access_token") &&
                               result.body["tenant_access_token"].is_string()
                           ? result.body["tenant_access_token"].get<std::string>()
                           : std::string{};
    if (result.ok && !token.empty()) {
        long long seconds = 7200;
        if (result.body.contains("expire") && result.body["expire"].is_number_integer())
            seconds = std::max<long long>(1, result.body["expire"].get<long long>());
        // 官方 SDK 提前 10 分钟刷新;寿命本身很短时提前一半。
        const auto margin = std::min<long long>(600, seconds / 2);
        token_ = token;
        refresh_at_ = Clock::now() + std::chrono::seconds(std::max<long long>(seconds - margin, 1));
        return token_;
    }
    if (result.body.is_object() && result.body.contains("code") && result.code != 0 && !is_rate_limited(result) &&
        response.status < 500 && !(result.code >= 50000 && result.code <= 59999)) {
        // 这个接口返回非 0 code 即凭据有问题(无效、停用、选错飞书 / Lark)。
        auth_failed_ = true;
        if (error) *error = redact(describe_token_error(result.code, result.msg));
        return {};
    }
    if (error) {
        *error = response.status >= 500 || is_rate_limited(result)
                     ? "飞书开放平台暂时不可用(HTTP " + std::to_string(response.status) + ")"
                     : std::string("飞书返回了无效的令牌响应");
    }
    return {};
}

std::string Api::tenant_token(std::string* error, const std::atomic<bool>* cancel) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!token_.empty() && Clock::now() < refresh_at_) return token_;
    return fetch_token_locked(error, cancel);
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

ApiResult Api::call(const std::string& method, const std::string& path, const nlohmann::json& body,
                    const std::atomic<bool>* cancel, std::chrono::milliseconds timeout) {
    ApiResult result;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string error;
        const auto token = tenant_token(&error, cancel);
        if (token.empty()) {
            result = ApiResult{};
            result.error = error;
            result.auth_failed = last_token_auth_failed();
            result.cancelled = cancel && cancel->load();
            return result;
        }
        HttpRequest request;
        request.method = method;
        request.url = url(path);
        request.headers = {{"Authorization", "Bearer " + token}, {"User-Agent", feishu_user_agent()}};
        if (!body.is_null()) {
            request.headers.emplace_back("Content-Type", "application/json; charset=utf-8");
            request.body = dump_json(body);
        }
        request.timeout = timeout;
        request.use_proxy = options_.use_proxy;
        request.cancel = cancel_check(cancel);
        const auto response = http_send(request);
        result = parse_api_response(response.status, response.body);
        result.cancelled = response.cancelled;
        attach_response_headers(result, response);
        if (response.status == 0) result.error = redact(response.error);
        if (is_token_invalid(result.code) && attempt == 0) {
            invalidate_token();
            continue;
        }
        result.msg = redact(result.msg);
        return result;
    }
    return result;
}

ApiResult Api::upload(const std::string& path, const std::vector<HttpPart>& parts) {
    ApiResult result;
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string error;
        const auto token = tenant_token(&error);
        if (token.empty()) {
            result = ApiResult{};
            result.error = error;
            result.auth_failed = last_token_auth_failed();
            return result;
        }
        HttpRequest request;
        request.method = "POST";
        request.url = url(path);
        request.headers = {{"Authorization", "Bearer " + token}, {"User-Agent", feishu_user_agent()}};
        request.parts = parts;
        request.timeout = std::chrono::minutes(5);
        request.use_proxy = options_.use_proxy;
        const auto response = http_send(request);
        result = parse_api_response(response.status, response.body);
        attach_response_headers(result, response);
        if (response.status == 0) result.error = redact(response.error);
        if (is_token_invalid(result.code) && attempt == 0) {
            invalidate_token();
            continue;
        }
        result.msg = redact(result.msg);
        return result;
    }
    return result;
}

BotInfo Api::bot_info(std::string* error, const std::atomic<bool>* cancel) {
    const auto result = call("GET", "/open-apis/bot/v3/info", nullptr, cancel, std::chrono::seconds(15));
    if (!result.ok) {
        if (error) *error = describe_error(result);
        return {};
    }
    auto info = parse_bot_info(result.body);
    if (!info.ok && error) *error = describe_activate_status(-1);
    return info;
}

EndpointInfo Api::ws_endpoint(const std::atomic<bool>* cancel) {
    HttpRequest request;
    request.method = "POST";
    request.url = url("/callback/ws/endpoint");
    // 两个 SDK 都带 locale: zh,错误信息会以中文返回。
    request.headers = {{"Content-Type", "application/json"}, {"locale", "zh"}, {"User-Agent", feishu_user_agent()}};
    request.body = dump_json({{"AppID", options_.app_id}, {"AppSecret", options_.app_secret}});
    request.timeout = std::chrono::seconds(15);
    request.use_proxy = options_.use_proxy;
    request.cancel = cancel_check(cancel);
    const auto response = http_send(request);
    if (response.status == 0) {
        EndpointInfo info;
        info.message = response.cancelled
                           ? std::string("已取消")
                           : redact("无法连接飞书开放平台" +
                                    (response.error.empty() ? std::string{} : ":" + response.error));
        return info;
    }
    auto info = parse_endpoint_response(response.status, response.body);
    info.message = redact(info.message);
    return info;
}

bool Api::download_resource(const ResourceRef& ref, const std::filesystem::path& dest, std::uint64_t max_bytes,
                            std::string* error) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string token_error;
        const auto token = tenant_token(&token_error);
        if (token.empty()) {
            if (error) *error = "下载附件失败:" + token_error;
            return false;
        }
        HttpRequest request;
        request.url = url("/open-apis/im/v1/messages/" + path_segment(ref.message_id) + "/resources/" +
                          path_segment(ref.key) + "?type=" + path_segment(ref.type));
        request.headers = {{"Authorization", "Bearer " + token}, {"User-Agent", feishu_user_agent()}};
        request.timeout = std::chrono::minutes(5);
        request.use_proxy = options_.use_proxy;
        request.download_to = dest;
        request.max_download_bytes = max_bytes;
        const auto response = http_send(request);
        if (response.too_large) {
            if (error) *error = "附件超过大小限制";
            return false;
        }
        if (response.status >= 200 && response.status < 300) return true;
        const auto body = read_head(dest, 64 * 1024);
        std::error_code ec;
        std::filesystem::remove(dest, ec);
        if (response.status == 0) {
            if (error)
                *error = redact("下载附件失败:无法连接飞书开放平台" +
                                (response.error.empty() ? std::string{} : ":" + response.error));
            return false;
        }
        const auto result = parse_api_response(response.status, body);
        if (is_token_invalid(result.code) && attempt == 0) {
            invalidate_token();
            continue;
        }
        if (error) *error = "下载附件失败:" + redact(describe_error(result));
        return false;
    }
    if (error) *error = "下载附件失败:飞书访问凭证失效";
    return false;
}

VerifyResult verify_credentials(const ApiOptions& options) {
    VerifyResult result;
    if (options.app_id.empty() || options.app_secret.empty()) {
        result.auth_failed = true;
        result.error = "请填写 App ID 和 App Secret";
        return result;
    }
    Api api(options);
    std::string error;
    if (api.tenant_token(&error).empty()) {
        result.auth_failed = api.last_token_auth_failed();
        result.network_error = !result.auth_failed;
        result.error = error.empty() ? std::string("凭据校验失败") : error;
        return result;
    }
    result.ok = true;
    const auto bot = api.bot_info(&error);
    if (bot.ok) {
        result.bot_open_id = bot.open_id;
        result.bot_name = bot.name;
        result.activate_status = bot.activate_status;
        result.bot_ready = bot.ready();
        if (!result.bot_ready) result.bot_warning = describe_activate_status(bot.activate_status);
    } else {
        result.bot_warning = error.empty() ? describe_activate_status(-1) : error;
    }
    return result;
}

} // namespace acecode::im::feishu
