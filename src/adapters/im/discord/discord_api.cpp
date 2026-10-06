#include "discord_api.hpp"

#include "im/redact.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <random>

namespace acecode::im::discord {
namespace {

std::string trim(const std::string& text) {
    std::size_t b = 0, e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) --e;
    return text.substr(b, e - b);
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

// 发消息的幂等键:最多 25 个字符,这里用 19 位以内的随机数字。
std::string make_nonce() {
    std::random_device device;
    const std::uint64_t value = (static_cast<std::uint64_t>(device()) << 32) ^ device();
    return std::to_string(value % 1000000000000000000ULL);
}

std::string string_field(const nlohmann::json& value, const char* key) {
    if (!value.is_object()) return {};
    const auto it = value.find(key);
    if (it == value.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<std::int64_t>());
    return {};
}

} // namespace

std::string normalize_token(std::string token) {
    token = trim(token);
    if (token.size() > 4 && (token.compare(0, 4, "Bot ") == 0 || token.compare(0, 4, "bot ") == 0))
        token = trim(token.substr(4));
    return token;
}

Api::Api(ApiOptions options, RateLimitPolicy policy) : options_(std::move(options)), policy_(policy) {
    while (!options_.api_base.empty() && options_.api_base.back() == '/') options_.api_base.pop_back();
    token_ = normalize_token(options_.token);
}

std::string Api::url(const std::string& path) const { return options_.api_base + path; }

void Api::cancel() {
    {
        std::lock_guard<std::mutex> lock(rl_mu_);
        cancelled_ = true;
    }
    rl_cv_.notify_all();
}

void Api::reset() {
    std::lock_guard<std::mutex> lock(rl_mu_);
    cancelled_ = false;
}

bool Api::wait_for_route(const std::string& route) {
    std::unique_lock<std::mutex> lock(rl_mu_);
    while (true) {
        if (cancelled_) return false;
        auto until = global_until_;
        const auto it = route_until_.find(route);
        if (it != route_until_.end()) until = (std::max)(until, it->second);
        if (until <= Clock::now()) return true;
        rl_cv_.wait_until(lock, until);
    }
}

bool Api::sleep_for(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(rl_mu_);
    rl_cv_.wait_for(lock, duration, [&cancelled = cancelled_] { return cancelled.load(); });
    return !cancelled_;
}

void Api::note_rate_limit(const std::string& route, std::chrono::milliseconds wait, bool global) {
    std::lock_guard<std::mutex> lock(rl_mu_);
    const auto now = Clock::now();
    const auto until = now + wait;
    if (global) {
        global_until_ = (std::max)(global_until_, until);
        return;
    }
    auto& slot = route_until_[route];
    slot = (std::max)(slot, until);
    if (route_until_.size() > 256) {
        for (auto it = route_until_.begin(); it != route_until_.end();) {
            if (it->second <= now) it = route_until_.erase(it);
            else ++it;
        }
    }
}

ApiResult Api::perform(HttpRequest request, const std::string& route) {
    ApiResult result;
    const std::vector<std::string> secrets{token_, options_.token};
    request.use_proxy = options_.use_proxy;
    request.headers.emplace_back("Authorization", "Bot " + token_);
    request.headers.emplace_back("User-Agent", discord_user_agent());
    request.cancel = [&cancelled = cancelled_] { return cancelled.load(); };
    for (int attempt = 0;; ++attempt) {
        if (!wait_for_route(route)) {
            result.cancelled = true;
            result.error.message = "Request cancelled";
            return result;
        }
        const auto response = http_send(request);
        result.status = response.status;
        if (response.cancelled || cancelled_) {
            result.cancelled = true;
            result.error = ApiError{};
            result.error.message = "Request cancelled";
            return result;
        }
        if (response.status == 0) {
            result.error = ApiError{};
            result.error.message = redact_secrets(response.error.empty() ? "HTTP request failed" : response.error, secrets);
            return result;
        }
        if (response.status >= 200 && response.status < 300) {
            result.ok = true;
            result.error = ApiError{};
            result.error.status = response.status;
            if (!trim(response.body).empty()) {
                try {
                    result.body = nlohmann::json::parse(response.body);
                } catch (...) {
                    result.body = nlohmann::json::object();
                }
            }
            return result;
        }
        result.error = parse_api_error(response.status, response.body);
        result.error.message = redact_secrets(result.error.message, secrets);
        result.error.detail = redact_secrets(result.error.detail, secrets);
        if (response.status != 429 || attempt >= policy_.max_retries) return result;
        // retry_after 是精确值(秒,小数);缺失时(例如 Cloudflare 的 HTML 页)按 1 秒处理。
        const double seconds = result.error.retry_after > 0 ? result.error.retry_after : 1.0;
        const auto wait = std::chrono::milliseconds(static_cast<long long>(std::ceil(seconds * 1000.0)));
        if (wait > policy_.max_wait) {
            LOG_WARN("[channels/discord] rate limited on " + route + " for " + std::to_string(wait.count()) +
                     " ms, giving up on this request");
            return result;
        }
        note_rate_limit(route, wait, result.error.global);
        LOG_INFO("[channels/discord] rate limited on " + route + (result.error.global ? " (global)" : "") +
                 ", retrying in " + std::to_string(wait.count()) + " ms");
    }
}

ApiResult Api::get(const std::string& path, std::chrono::milliseconds timeout) {
    HttpRequest request;
    request.url = url(path);
    request.timeout = timeout;
    return perform(std::move(request), "GET " + path);
}

ApiResult Api::post(const std::string& path, const nlohmann::json& body, std::chrono::milliseconds timeout) {
    HttpRequest request;
    request.method = "POST";
    request.url = url(path);
    request.timeout = timeout;
    if (!body.is_null()) {
        request.headers.emplace_back("Content-Type", "application/json");
        request.body = body.dump();
    }
    return perform(std::move(request), "POST " + path);
}

ApiResult Api::post_multipart(const std::string& path, std::vector<HttpPart> parts,
                              std::chrono::milliseconds timeout) {
    HttpRequest request;
    request.method = "POST";
    request.url = url(path);
    request.timeout = timeout;
    request.parts = std::move(parts);
    return perform(std::move(request), "POST " + path);
}

ApiResult Api::current_user() { return get("/users/@me", std::chrono::seconds(15)); }

ApiResult Api::current_application() { return get("/applications/@me", std::chrono::seconds(15)); }

ApiResult Api::gateway_bot() { return get("/gateway/bot", std::chrono::seconds(15)); }

ApiResult Api::open_dm(const std::string& user_id) {
    return post("/users/@me/channels", nlohmann::json{{"recipient_id", user_id}}, std::chrono::seconds(20));
}

ApiResult Api::get_message(const std::string& channel_id, const std::string& message_id) {
    return get("/channels/" + path_segment(channel_id) + "/messages/" + path_segment(message_id),
               std::chrono::seconds(20));
}

ApiResult Api::trigger_typing(const std::string& channel_id) {
    return post("/channels/" + path_segment(channel_id) + "/typing", nlohmann::json(), std::chrono::seconds(10));
}

ApiResult Api::send_with_nonce(const std::string& path, nlohmann::json body, bool multipart,
                               const std::filesystem::path& file, const std::string& filename,
                               const std::string& mime_type) {
    body["nonce"] = make_nonce();
    body["enforce_nonce"] = true;
    ApiResult result;
    for (int attempt = 0;; ++attempt) {
        if (multipart) {
            std::vector<HttpPart> parts;
            HttpPart payload;
            payload.name = "payload_json";
            payload.value = body.dump();
            payload.content_type = "application/json";
            parts.push_back(std::move(payload));
            HttpPart upload;
            upload.name = "files[0]";
            upload.file = file;
            upload.filename = filename;
            upload.content_type = mime_type.empty() ? "application/octet-stream" : mime_type;
            parts.push_back(std::move(upload));
            result = post_multipart(path, std::move(parts));
        } else {
            result = post(path, body);
        }
        if (result.ok || result.cancelled) return result;
        const bool transient = result.status == 0 || result.status >= 500;
        if (!transient || attempt >= policy_.max_send_retries) return result;
        LOG_INFO("[channels/discord] send failed (" +
                 (result.status == 0 ? std::string("network error") : "HTTP " + std::to_string(result.status)) +
                 "), resending with the same nonce");
        if (!sleep_for(policy_.send_retry_delay * (attempt + 1))) {
            result.cancelled = true;
            return result;
        }
    }
}

ApiResult Api::create_message(const std::string& channel_id, nlohmann::json body) {
    return send_with_nonce("/channels/" + path_segment(channel_id) + "/messages", std::move(body), false, {}, {}, {});
}

ApiResult Api::create_message_with_file(const std::string& channel_id, nlohmann::json payload,
                                        const std::filesystem::path& file, const std::string& filename,
                                        const std::string& mime_type) {
    return send_with_nonce("/channels/" + path_segment(channel_id) + "/messages", std::move(payload), true, file,
                           filename, mime_type);
}

DownloadResult Api::download(const std::string& url, const std::filesystem::path& dest, std::uint64_t max_bytes) {
    DownloadResult result;
    HttpRequest request;
    request.url = url;
    request.timeout = std::chrono::minutes(2);
    request.use_proxy = options_.use_proxy;
    request.download_to = dest;
    request.max_download_bytes = max_bytes;
    request.cancel = [&cancelled = cancelled_] { return cancelled.load(); };
    const auto response = http_send(request);
    result.status = response.status;
    result.too_large = response.too_large;
    result.cancelled = response.cancelled;
    if (!response.too_large && response.status >= 200 && response.status < 300 && !response.cancelled) {
        result.ok = true;
        return result;
    }
    std::error_code ec;
    if (!response.too_large) std::filesystem::remove(dest, ec);
    // curl 的错误文本可能带完整 URL(含签名),只保留主机名之前的部分不可靠,干脆只给类别。
    result.error = response.too_large ? "Download exceeds limit"
                   : response.status == 0 ? "network error"
                                          : "HTTP " + std::to_string(response.status);
    return result;
}

ValidationResult validate_credentials(const ApiOptions& options) {
    ValidationResult result;
    ApiOptions normalized = options;
    normalized.token = normalize_token(options.token);
    if (normalized.token.empty()) {
        result.auth_failed = true;
        result.error = "请填写 Discord Bot Token";
        return result;
    }
    RateLimitPolicy policy;
    policy.max_retries = 1;
    policy.max_wait = std::chrono::seconds(10);
    Api api(normalized, policy);
    const auto me = api.current_user();
    if (!me.ok) {
        const auto& error = me.error;
        if (me.status == 401 || error.code == 40001) {
            result.auth_failed = true;
            result.error = "Bot Token 无效或已被重置,请在 Discord 开发者后台的 Bot 页面重新生成并复制";
        } else if (me.status == 403 && error.code != 0) {
            result.auth_failed = true;
            result.error = "Discord 拒绝了这个机器人账号:" + error.message;
        } else if (me.status == 403) {
            result.error = "请求被 Discord 拦截(HTTP 403),可能是当前网络出口被限制,请检查代理";
        } else if (me.status == 0) {
            result.error = error.message.empty() ? "无法连接 Discord,请检查网络或代理"
                                                 : "无法连接 Discord,请检查网络或代理(" + error.message + ")";
        } else {
            result.error = describe_error(error);
        }
        return result;
    }
    result.profile.user_id = string_field(me.body, "id");
    result.profile.username = string_field(me.body, "username");
    if (result.profile.user_id.empty()) {
        result.error = "Discord 返回的机器人信息无效";
        return result;
    }
    const auto app = api.current_application();
    if (app.ok) {
        result.profile.application_id = string_field(app.body, "id");
        result.profile.application_name = string_field(app.body, "name");
        result.profile.message_content = message_content_intent(app.body);
    } else {
        LOG_WARN("[channels/discord] GET /applications/@me failed during validation (HTTP " +
                 std::to_string(app.status) + "), intent state unknown");
    }
    result.profile.invite_url = invite_url(result.profile.application_id);
    result.ok = true;
    return result;
}

} // namespace acecode::im::discord
