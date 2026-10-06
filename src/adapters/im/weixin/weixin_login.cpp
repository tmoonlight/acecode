#include "weixin_login.hpp"

#include "im/weixin/weixin_api.hpp"
#include "utils/logger.hpp"

#include <thread>

namespace acecode::im::weixin {
namespace {

using Clock = std::chrono::steady_clock;

// 等待 duration;期间每 50ms 检查一次取消。返回 false 表示被取消。
bool wait_or_cancel(std::chrono::milliseconds duration, const std::atomic<bool>& cancelled) {
    const auto until = Clock::now() + duration;
    while (Clock::now() < until) {
        if (cancelled) return false;
        std::this_thread::sleep_for((std::min<std::chrono::milliseconds>)(
            std::chrono::milliseconds(50), std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now())));
    }
    return !cancelled;
}

std::chrono::milliseconds bounded(std::chrono::milliseconds wanted, Clock::time_point deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    if (remaining < std::chrono::milliseconds(1000)) return std::chrono::milliseconds(1000);
    return (std::min)(wanted, remaining);
}

std::string string_field(const nlohmann::json& body, const char* key) {
    if (!body.is_object() || !body.contains(key) || !body[key].is_string()) return {};
    return body[key].get<std::string>();
}

std::string normalized_base(const std::string& value, const std::string& fallback) {
    std::string base = value;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.rfind("https://", 0) == 0 || base.rfind("http://", 0) == 0) return base;
    return fallback;
}

LoginResult finish(LoginPhase phase, int refreshes, std::string error = {}) {
    LoginResult result;
    result.phase = phase;
    result.refreshes = refreshes;
    result.error = std::move(error);
    return result;
}

} // namespace

const char* login_phase_name(LoginPhase phase) {
    switch (phase) {
        case LoginPhase::WaitingScan: return "waiting";
        case LoginPhase::Scanned: return "scanned";
        case LoginPhase::Completed: return "completed";
        case LoginPhase::Failed: return "failed";
        case LoginPhase::Cancelled: return "cancelled";
        case LoginPhase::TimedOut: return "timed_out";
    }
    return "failed";
}

LoginResult run_login(const LoginOptions& options, const std::atomic<bool>& cancelled,
                      const LoginProgressFn& progress) {
    const auto deadline = Clock::now() + options.total_timeout;
    const std::function<bool()> cancel = [&cancelled] { return cancelled.load(); };
    const auto api_base = normalized_base(options.api_base, kApiBase);
    int refreshes = 0;
    while (true) {
        if (cancelled) return finish(LoginPhase::Cancelled, refreshes);
        if (Clock::now() >= deadline) return finish(LoginPhase::TimedOut, refreshes);

        // 二维码一律从默认地址申请(换码时也不沿用上一张的重定向地址)。
        const auto qr = get_unauthenticated(join_url(api_base, kEpGetBotQr) + "?bot_type=" + kBotType,
                                            options.channel_version, options.use_proxy,
                                            bounded(options.qr_timeout, deadline), cancel);
        if (cancelled) return finish(LoginPhase::Cancelled, refreshes);
        if (!qr.ok) return finish(LoginPhase::Failed, refreshes, "获取微信二维码失败:" + qr.error);
        const auto qrcode = string_field(qr.body, "qrcode");
        if (qrcode.empty()) return finish(LoginPhase::Failed, refreshes, "获取微信二维码失败:返回内容缺少二维码");
        const auto image = string_field(qr.body, "qrcode_img_content");
        LoginProgress shown;
        shown.phase = LoginPhase::WaitingScan;
        shown.qr_url = image.empty() ? qrcode : image;
        shown.refreshes = refreshes;
        LOG_INFO("[channels/weixin] login QR issued (refreshes=" + std::to_string(refreshes) + ")");
        if (progress) progress(shown);

        auto poll_base = api_base;
        bool scanned = false;
        bool expired = false;
        while (!expired) {
            if (cancelled) return finish(LoginPhase::Cancelled, refreshes);
            if (Clock::now() >= deadline) return finish(LoginPhase::TimedOut, refreshes);
            const auto polled = get_unauthenticated(
                join_url(poll_base, kEpGetQrStatus) + "?qrcode=" + url_encode(qrcode), options.channel_version,
                options.use_proxy, bounded(options.status_timeout, deadline), cancel);
            if (cancelled) return finish(LoginPhase::Cancelled, refreshes);
            // 客户端超时、网络抖动、平台临时错误都按“继续等待”处理(与 hermes / 官方插件一致)。
            const auto status = polled.ok ? string_field(polled.body, "status") : std::string("wait");
            if (status == "scaned" || status == "scaned_but_redirect") {
                if (status == "scaned_but_redirect") {
                    const auto host = string_field(polled.body, "redirect_host");
                    if (!host.empty()) {
                        poll_base = options.redirect_scheme + "://" + host;
                        LOG_INFO("[channels/weixin] login status polling switched to a redirected host");
                    }
                }
                if (!scanned) {
                    scanned = true;
                    LOG_INFO("[channels/weixin] login QR scanned, waiting for confirmation");
                    LoginProgress step = shown;
                    step.phase = LoginPhase::Scanned;
                    if (progress) progress(step);
                }
            } else if (status == "need_verifycode" || status == "verify_code_blocked") {
                LOG_WARN("[channels/weixin] login requires a verification code, which is not supported");
                return finish(LoginPhase::Failed, refreshes,
                              "微信要求输入手机上显示的验证数字,ACECode 暂不支持这种确认方式,请稍后重新扫码");
            } else if (status == "binded_redirect") {
                LOG_WARN("[channels/weixin] login answered binded_redirect without credentials");
                return finish(LoginPhase::Failed, refreshes, "微信提示该账号已连接过,但没有下发新的凭据,请稍后重新扫码");
            } else if (status == "expired") {
                ++refreshes;
                if (refreshes > options.max_refreshes)
                    return finish(LoginPhase::Failed, refreshes, "二维码多次过期,请重新开始扫码");
                LOG_INFO("[channels/weixin] login QR expired, requesting a fresh one");
                expired = true;
                continue;
            } else if (status == "confirmed") {
                LoginResult done;
                done.refreshes = refreshes;
                done.bot_token = string_field(polled.body, "bot_token");
                done.bot_id = id_string(polled.body.value("ilink_bot_id", nlohmann::json()));
                done.user_id = id_string(polled.body.value("ilink_user_id", nlohmann::json()));
                done.base_url = normalized_base(string_field(polled.body, "baseurl"), api_base);
                if (done.bot_token.empty() || done.bot_id.empty() || !valid_id(done.bot_id)) {
                    LOG_WARN("[channels/weixin] login confirmed without usable credentials");
                    return finish(LoginPhase::Failed, refreshes, "扫码结果缺少机器人凭据,请重新扫码");
                }
                if (!done.user_id.empty() && !valid_id(done.user_id)) done.user_id.clear();
                done.phase = LoginPhase::Completed;
                LOG_INFO("[channels/weixin] login confirmed");
                return done;
            }
            // wait 与未知状态:间隔后继续轮询。
            if (!wait_or_cancel(options.poll_interval, cancelled)) return finish(LoginPhase::Cancelled, refreshes);
        }
    }
}

} // namespace acecode::im::weixin
