#include "tg_api.hpp"

#include "im/redact.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <vector>

namespace acecode::im::telegram {
namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

ConflictKind conflict_kind(const ApiResult& result) {
    if (result.status != 409 && result.error_code != 409) return ConflictKind::None;
    return lower(result.description).find("webhook") != std::string::npos ? ConflictKind::Webhook
                                                                           : ConflictKind::OtherPoller;
}

Api::Api(ApiOptions options) : options_(std::move(options)) {
    while (!options_.api_base.empty() && options_.api_base.back() == '/') options_.api_base.pop_back();
}

std::string Api::base() const { return options_.api_base + "/bot" + options_.token + "/"; }

ApiResult Api::parse(const HttpResponse& response) const {
    ApiResult result;
    result.status = response.status;
    result.cancelled = response.cancelled;
    const std::vector<std::string> secrets{options_.token};
    if (response.status == 0) {
        result.description = redact_secrets(response.error.empty() ? "无法连接 Telegram" : response.error, secrets);
        return result;
    }
    try {
        const auto json = nlohmann::json::parse(response.body);
        if (!json.is_object()) throw std::runtime_error("not an object");
        result.ok = json.value("ok", false) && response.status >= 200 && response.status < 300;
        if (json.contains("result")) result.result = json["result"];
        result.error_code = json.value("error_code", 0);
        result.description = redact_secrets(json.value("description", std::string{}), secrets);
        if (json.contains("parameters") && json["parameters"].is_object())
            result.retry_after = json["parameters"].value("retry_after", 0);
    } catch (...) {
        result.ok = false;
        result.description = "Telegram 返回了无效数据(HTTP " + std::to_string(response.status) + ")";
    }
    if (!result.ok && result.description.empty())
        result.description = "Telegram 请求失败(HTTP " + std::to_string(response.status) + ")";
    return result;
}

ApiResult Api::call(const std::string& method, const nlohmann::json& params, std::chrono::milliseconds timeout,
                    std::function<bool()> cancel) const {
    HttpRequest request;
    request.method = "POST";
    request.url = base() + method;
    request.headers = {{"Content-Type", "application/json"}};
    request.body = params.is_null() ? "{}" : params.dump();
    request.timeout = timeout;
    request.use_proxy = options_.use_proxy;
    request.cancel = std::move(cancel);
    return parse(http_send(request));
}

ApiResult Api::call_multipart(const std::string& method, std::vector<HttpPart> parts,
                              std::chrono::milliseconds timeout) const {
    HttpRequest request;
    request.method = "POST";
    request.url = base() + method;
    request.parts = std::move(parts);
    request.timeout = timeout;
    request.use_proxy = options_.use_proxy;
    return parse(http_send(request));
}

bool Api::download(const std::string& file_path, const std::filesystem::path& dest, std::uint64_t max_bytes,
                   std::string* error) const {
    HttpRequest request;
    request.url = options_.api_base + "/file/bot" + options_.token + "/" + file_path;
    request.timeout = std::chrono::minutes(2);
    request.use_proxy = options_.use_proxy;
    request.download_to = dest;
    request.max_download_bytes = max_bytes;
    const auto response = http_send(request);
    if (response.too_large) {
        if (error) *error = "文件超过 Telegram 下载上限";
        return false;
    }
    if (response.status < 200 || response.status >= 300) {
        std::error_code ec;
        std::filesystem::remove(dest, ec);
        if (error) {
            *error = response.status == 0 ? redact_secrets(response.error, {options_.token})
                                          : "下载文件失败:HTTP " + std::to_string(response.status);
        }
        return false;
    }
    return true;
}

} // namespace acecode::im::telegram
