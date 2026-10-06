#include "qq_api.hpp"

#include "im/http.hpp"
#include "im/redact.hpp"

#include <algorithm>
#include <vector>

namespace acecode::im::qqbot {
namespace {

std::string trim_slash(std::string base) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base;
}

std::string path_segment(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : value) {
        const auto u = static_cast<unsigned char>(c);
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') ||
            u == '-' || u == '_' || u == '.' || u == '~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 0x0F]);
        }
    }
    return out;
}

std::string entity_base(const std::string& scope, const std::string& target) {
    return (scope == "group" ? "/v2/groups/" : "/v2/users/") + path_segment(target);
}

long long expires_seconds(const nlohmann::json& value) {
    if (value.is_number_integer()) return value.get<long long>();
    if (value.is_string()) {
        try {
            return std::stoll(value.get<std::string>());
        } catch (...) {
        }
    }
    return 7200;
}

} // namespace

Api::Api(ApiOptions options) : options_(std::move(options)) {}

std::string Api::fetch_token_locked(std::string* error) {
    HttpRequest request;
    request.method = "POST";
    request.url = options_.token_url;
    request.headers = {{"Content-Type", "application/json"}};
    request.body = nlohmann::json{{"appId", options_.app_id}, {"clientSecret", options_.app_secret}}.dump();
    request.timeout = std::chrono::seconds(15);
    request.use_proxy = options_.use_proxy;
    const auto response = http_send(request);
    const std::vector<std::string> secrets{options_.app_secret};
    auth_failed_ = false;
    if (response.status == 0) {
        if (error) *error = redact_secrets(response.error.empty() ? "无法连接 QQ 开放平台" : response.error, secrets);
        return {};
    }
    // 失败时平台仍可能返回 HTTP 200,以响应里有没有 access_token 为准。
    try {
        const auto json = nlohmann::json::parse(response.body);
        if (json.is_object() && json.contains("access_token") && json["access_token"].is_string() &&
            !json["access_token"].get<std::string>().empty()) {
            token_ = json["access_token"].get<std::string>();
            const auto seconds = expires_seconds(json.value("expires_in", nlohmann::json(7200)));
            const auto margin = std::min<long long>(300, std::max<long long>(seconds / 2, 1));
            refresh_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(std::max<long long>(seconds - margin, 1));
            return token_;
        }
        const auto api_error = parse_api_error(response.status, response.body);
        auth_failed_ = response.status < 500;
        if (error) *error = redact_secrets("凭据无效:" + api_error.message, secrets);
    } catch (...) {
        if (error) *error = "QQ 开放平台返回了无效的令牌响应";
    }
    return {};
}

std::string Api::access_token(std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!token_.empty() && std::chrono::steady_clock::now() < refresh_at_) return token_;
    return fetch_token_locked(error);
}

bool Api::last_token_auth_failed() const {
    std::lock_guard<std::mutex> lock(mu_);
    return auth_failed_;
}

void Api::invalidate_token() {
    std::lock_guard<std::mutex> lock(mu_);
    token_.clear();
    refresh_at_ = {};
}

bool Api::verify(std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    token_.clear();
    return !fetch_token_locked(error).empty();
}

std::string Api::gateway_url(std::string* error) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto token = access_token(error);
        if (token.empty()) return {};
        HttpRequest request;
        request.url = trim_slash(options_.api_base) + "/gateway";
        request.headers = {{"Authorization", "QQBot " + token}};
        request.timeout = std::chrono::seconds(15);
        request.use_proxy = options_.use_proxy;
        const auto response = http_send(request);
        if (response.status == 401 && attempt == 0) {
            invalidate_token();
            continue;
        }
        if (response.status != 200) {
            if (error) *error = response.status == 0 ? response.error
                                                     : "获取网关地址失败:" + parse_api_error(response.status, response.body).message;
            return {};
        }
        try {
            const auto json = nlohmann::json::parse(response.body);
            const auto url = json.value("url", std::string{});
            if (!url.empty()) return url;
        } catch (...) {
        }
        if (error) *error = "QQ 网关地址无效";
        return {};
    }
    if (error) *error = "QQ 鉴权失败";
    return {};
}

ApiResult Api::post(const std::string& path, const nlohmann::json& body) {
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
        request.url = trim_slash(options_.api_base) + path;
        request.headers = {{"Content-Type", "application/json"}, {"Authorization", "QQBot " + token}};
        request.body = body.dump();
        request.timeout = std::chrono::seconds(60);
        request.use_proxy = options_.use_proxy;
        const auto response = http_send(request);
        if (response.status == 401 && attempt == 0) {
            invalidate_token();
            continue;
        }
        if (response.status >= 200 && response.status < 300) {
            result.ok = true;
            try {
                result.response = nlohmann::json::parse(response.body);
            } catch (...) {
                result.response = nlohmann::json::object();
            }
            return result;
        }
        result.error = response.status == 0 ? ApiError{0, 0, response.error}
                                            : parse_api_error(response.status, response.body);
        result.error.message = redact_secrets(result.error.message, {options_.app_secret});
        return result;
    }
    return result;
}

ApiResult Api::send_message(const std::string& scope, const std::string& target, const nlohmann::json& body) {
    return post(entity_base(scope, target) + "/messages", body);
}

ApiResult Api::upload_file(const std::string& scope, const std::string& target, int file_type,
                           const std::string& file_base64, const std::string& file_name) {
    nlohmann::json body{{"file_type", file_type}, {"srv_send_msg", false}, {"file_data", file_base64}};
    if (file_type == kFileTypeFile && !file_name.empty()) body["file_name"] = file_name;
    return post(entity_base(scope, target) + "/files", body);
}

bool Api::download(const std::string& url, const std::filesystem::path& dest, std::uint64_t max_bytes,
                   std::string* error) {
    std::string token_error;
    const auto token = access_token(&token_error);
    HttpRequest request;
    request.url = normalize_attachment_url(url);
    if (!token.empty()) request.headers = {{"Authorization", "QQBot " + token}};
    request.timeout = std::chrono::seconds(60);
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
        if (error) *error = response.status == 0 ? response.error : "下载附件失败:HTTP " + std::to_string(response.status);
        return false;
    }
    return true;
}

} // namespace acecode::im::qqbot
