#include "weixin_api.hpp"

#include "im/redact.hpp"
#include "platform/crypto/secure_random.hpp"


#include <algorithm>
#include <random>

namespace acecode::im::weixin {
namespace {

using Clock = std::chrono::steady_clock;

std::uint32_t random_uin() {
    const auto bytes = platform::secure_random_bytes(4);
    if (bytes.size() == 4) {
        return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[0])) << 24) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[1])) << 16) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[2])) << 8) |
               static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[3]));
    }
    // 这个值不是凭据(平台只要求它是随机数),系统随机源不可用时退回普通随机数即可。
    std::random_device device;
    return static_cast<std::uint32_t>(device());
}

ApiResult interpret(const HttpResponse& response, Clock::duration elapsed, std::chrono::milliseconds timeout,
                    const std::vector<std::string>& secrets) {
    ApiResult result;
    result.status = response.status;
    result.cancelled = response.cancelled;
    if (response.cancelled) {
        result.error = "请求已取消";
        return result;
    }
    if (response.status == 0) {
        const auto margin = std::chrono::milliseconds(1000);
        result.timed_out = timeout > margin && elapsed >= timeout - margin;
        const auto detail = redact_secrets(response.error, secrets);
        result.error = detail.empty() ? std::string("无法连接微信服务器") : "无法连接微信服务器:" + detail;
        return result;
    }
    const auto envelope = parse_envelope(response.body);
    if (response.status < 200 || response.status >= 300) {
        result.error = "微信服务器返回 HTTP " + std::to_string(response.status);
        return result;
    }
    if (!envelope.parsed) {
        result.error = "微信返回了无效数据";
        return result;
    }
    result.ret = envelope.ret;
    result.errcode = envelope.errcode;
    result.errmsg = redact_secrets(envelope.errmsg, secrets);
    if (envelope_failed(envelope)) {
        result.session_expired = is_session_expired(envelope.ret, envelope.errcode, envelope.errmsg);
        result.rate_limited = is_rate_limited(envelope.ret, envelope.errcode, envelope.errmsg);
        if (result.session_expired) {
            result.error = kSessionExpiredText;
        } else if (result.rate_limited) {
            result.error = "微信发送过于频繁,请稍后再试";
        } else {
            const int code = envelope.errcode != 0 ? envelope.errcode : envelope.ret;
            result.error = "微信拒绝了请求(错误码 " + std::to_string(code) +
                           (result.errmsg.empty() ? std::string{} : ":" + result.errmsg) + ")";
        }
        return result;
    }
    result.ok = true;
    result.body = envelope.body;
    return result;
}

std::vector<std::pair<std::string, std::string>> common_headers(const std::string& channel_version) {
    return {{"iLink-App-Id", kAppId},
            {"iLink-App-ClientVersion", std::to_string(client_version_number(channel_version))}};
}

} // namespace

Api::Api(ApiOptions options) : options_(std::move(options)) {
    if (options_.base_url.empty()) options_.base_url = kApiBase;
    if (options_.cdn_base.empty()) options_.cdn_base = kCdnBase;
    if (options_.channel_version.empty()) options_.channel_version = kChannelVersion;
}

std::vector<std::pair<std::string, std::string>> Api::headers() const {
    auto headers = common_headers(options_.channel_version);
    headers.emplace_back("Content-Type", "application/json");
    headers.emplace_back("AuthorizationType", "ilink_bot_token");
    headers.emplace_back("X-WECHAT-UIN", wechat_uin(random_uin()));
    if (!options_.token.empty()) headers.emplace_back("Authorization", "Bearer " + options_.token);
    return headers;
}

ApiResult Api::post(const std::string& endpoint, nlohmann::json body, std::chrono::milliseconds timeout,
                    std::function<bool()> cancel) const {
    if (!body.is_object()) body = nlohmann::json::object();
    nlohmann::json base_info{{"channel_version", options_.channel_version}};
    if (!options_.bot_agent.empty()) base_info["bot_agent"] = options_.bot_agent;
    body["base_info"] = std::move(base_info);

    HttpRequest request;
    request.method = "POST";
    request.url = join_url(options_.base_url, endpoint);
    request.headers = headers();
    request.body = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    request.timeout = timeout;
    request.use_proxy = options_.use_proxy;
    request.cancel = std::move(cancel);
    const auto started = Clock::now();
    const auto response = http_send(request);
    return interpret(response, Clock::now() - started, timeout, {options_.token});
}

ApiResult get_unauthenticated(const std::string& url, const std::string& channel_version, bool use_proxy,
                              std::chrono::milliseconds timeout, std::function<bool()> cancel) {
    HttpRequest request;
    request.url = url;
    request.headers = common_headers(channel_version.empty() ? std::string(kChannelVersion) : channel_version);
    request.timeout = timeout;
    request.use_proxy = use_proxy;
    request.cancel = std::move(cancel);
    const auto started = Clock::now();
    const auto response = http_send(request);
    return interpret(response, Clock::now() - started, timeout, {});
}

// CDN 上传的结果只在响应头 x-encrypted-param 里(im::http_send 的响应头键已转小写)。
CdnUploadResult cdn_upload(const std::string& url, const std::string& ciphertext, bool use_proxy,
                           std::chrono::milliseconds timeout) {
    CdnUploadResult result;
    HttpRequest request;
    request.method = "POST";
    request.url = url;
    request.headers = {{"Content-Type", "application/octet-stream"}};
    request.body = ciphertext;
    request.timeout = timeout;
    request.use_proxy = use_proxy;
    const auto response = http_send(request);
    result.status = response.status;
    if (response.status == 0) {
        result.retryable = true;
        result.error = response.error.empty() ? std::string("上传文件失败:无法连接微信文件服务器")
                                              : "上传文件失败:" + response.error;
        return result;
    }
    const auto header_value = [&response](const char* name) {
        const auto it = response.headers.find(name);
        return it == response.headers.end() ? std::string{} : it->second;
    };
    if (response.status != 200) {
        result.retryable = response.status >= 500;
        const auto detail = redact_secrets(header_value("x-error-message"));
        result.error = "上传文件失败:HTTP " + std::to_string(response.status) +
                       (detail.empty() ? std::string{} : "(" + detail + ")");
        return result;
    }
    result.encrypted_param = header_value("x-encrypted-param");
    if (result.encrypted_param.empty()) {
        const auto detail = redact_secrets(header_value("x-error-message"));
        result.error = detail.empty() ? std::string("上传文件失败:文件服务器没有返回文件标识")
                                      : "上传文件失败:" + detail;
        return result;
    }
    result.ok = true;
    return result;
}

SessionCheck check_session(const ApiOptions& options, const std::string& user_id, std::chrono::milliseconds timeout) {
    SessionCheck check;
    if (options.token.empty()) {
        check.state = SessionState::Expired;
        check.error = "缺少微信登录凭据,请在设置页扫码登录";
        return check;
    }
    const auto result = Api(options).post(kEpGetConfig, {{"ilink_user_id", user_id}}, timeout);
    if (result.ok) {
        check.state = SessionState::Valid;
    } else if (result.session_expired) {
        check.state = SessionState::Expired;
        check.error = kSessionExpiredText;
    } else {
        check.state = SessionState::Unknown;
        check.error = result.error;
    }
    return check;
}

} // namespace acecode::im::weixin
