#pragma once

// 微信 iLink 扫码登录(微信 ClawBot)。流程按 hermes-agent 验证过的 2.2.0 版本:
//   1. GET {api}/ilink/bot/get_bot_qrcode?bot_type=3 → {qrcode, qrcode_img_content}
//      二维码要编码 qrcode_img_content(liteapp 网址),不是 32 位的 qrcode 令牌。
//   2. GET {base}/ilink/bot/get_qrcode_status?qrcode=<令牌>,服务端挂起约 30 秒:
//      wait 继续等;scaned 已扫码待确认;scaned_but_redirect 换到 https://{redirect_host} 继续轮询
//      同一个令牌;expired 换一张新二维码(从默认地址重新申请);confirmed 带回凭据。
//      新版协议的 need_verifycode / verify_code_blocked / binded_redirect 不支持,直接失败并说明。
//   3. confirmed:bot_token(之后所有请求的 Bearer)、ilink_bot_id(机器人账号)、baseurl
//      (之后的接口地址)、ilink_user_id(扫码人,即机主)。
// 二维码内容与 bot_token 绝不写日志。

#include "im/weixin/weixin_protocol.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

namespace acecode::im::weixin {

enum class LoginPhase { WaitingScan, Scanned, Completed, Failed, Cancelled, TimedOut };

// "waiting" / "scanned" / "completed" / "failed" / "cancelled" / "timed_out"
const char* login_phase_name(LoginPhase phase);

struct LoginProgress {
    LoginPhase phase = LoginPhase::WaitingScan;  // WaitingScan:新二维码;Scanned:已扫码,等手机上确认
    std::string qr_url;                           // 要渲染成二维码的内容(qrcode_img_content)
    int refreshes = 0;                            // 因过期自动换码的次数
};

struct LoginResult {
    LoginPhase phase = LoginPhase::Failed;  // Completed / Failed / Cancelled / TimedOut
    std::string error;                      // Failed:中文原因,不含凭据
    std::string bot_token;                  // Completed:只交给保存逻辑,不得展示或记录
    std::string bot_id;                     // Completed:ilink_bot_id(Address::account)
    std::string base_url;                   // Completed:之后使用的接口地址
    std::string user_id;                    // Completed:扫码人的 ilink_user_id(可能为空)
    int refreshes = 0;
};

struct LoginOptions {
    std::string api_base = kApiBase;  // 申请二维码与默认轮询地址
    bool use_proxy = true;
    std::chrono::milliseconds poll_interval{std::chrono::seconds(1)};  // 每次状态应答后的间隔
    std::chrono::milliseconds total_timeout{std::chrono::minutes(10)};
    std::chrono::milliseconds qr_timeout{std::chrono::seconds(35)};
    std::chrono::milliseconds status_timeout{std::chrono::seconds(35)};  // 状态长轮询的客户端超时
    // 二维码过期后最多自动换几次(10 分钟内最多 6 张);超过则失败。
    int max_refreshes = 5;
    std::string redirect_scheme = "https";  // scaned_but_redirect 的新地址协议(测试用 http)
    std::string channel_version = kChannelVersion;
};

using LoginProgressFn = std::function<void(const LoginProgress&)>;

// 同步执行完整扫码流程,由调用方放到后台线程。显示或刷新二维码时回调一次 WaitingScan,
// 首次扫码时回调一次 Scanned(qr_url 不变)。cancelled 置位后约 1 秒内返回 Cancelled。
LoginResult run_login(const LoginOptions& options, const std::atomic<bool>& cancelled,
                      const LoginProgressFn& progress);

} // namespace acecode::im::weixin
