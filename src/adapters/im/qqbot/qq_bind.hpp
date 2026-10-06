#pragma once

// QQ 机器人扫码配置(add-desktop-im-channels design D9)。
//
// 协议取自 WorkBuddy 5.6.2 内置的腾讯官方 @tencent-connect/qqbot-connector 1.1.0,
// 并与 Hermes qqbot/onboard.py 交叉印证(该包无开源许可,只参照协议,未复制代码):
//   1. POST {portal}/lite/create_bind_task {"key": base64(32 字节随机密钥)} → task_id
//   2. 二维码内容:{portal}/qqbot/openclaw/connect.html?task_id=..&source=acecode&_wv=2
//   3. 每 2 秒 POST {portal}/lite/poll_bind_result {"task_id"}:status 0 无 / 1 进行中 /
//      2 完成(附 bot_appid、bot_encrypt_secret、可能有 user_openid)/ 3 过期(换新任务)
//   4. bot_encrypt_secret = base64(IV 12 字节 | 密文 | 认证标签 16 字节),AES-256-GCM 解密
// 二维码内容与解密后的密钥绝不写日志。

#include "im/qqbot/qq_protocol.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

namespace acecode::im::qqbot {

enum class BindPhase { WaitingScan, Completed, Failed, Cancelled, TimedOut };

struct BindUpdate {
    BindPhase phase = BindPhase::WaitingScan;
    std::string qr_url;       // WaitingScan:当前二维码内容
    std::string error;        // Failed:原因,不含任何密钥
    std::string app_id;       // Completed
    std::string app_secret;   // Completed:只交给保存逻辑,不得展示或记录
    std::string user_openid;  // Completed:扫码人在单聊里的 openid,接口没给时为空
    int refreshes = 0;        // 二维码因过期刷新的次数
};

struct BindOptions {
    std::string portal_base = kPortalBase;   // 申请与轮询接口所在的门户
    std::string connect_base = kPortalBase;  // 二维码指向的门户(始终是正式门户)
    std::string source = kBindSource;
    bool use_proxy = true;
    std::chrono::milliseconds poll_interval{std::chrono::seconds(2)};
    std::chrono::milliseconds total_timeout{std::chrono::minutes(10)};
    std::chrono::milliseconds request_timeout{std::chrono::seconds(10)};
    // 测试注入点:返回 32 字节密钥;为空时使用密码学安全随机数。
    std::function<std::string()> key_provider;
};

using BindProgress = std::function<void(const BindUpdate&)>;

// 同步执行完整扫码流程,由调用方放到后台线程。每次显示或刷新二维码都会回调 progress;
// 返回最终结果(Completed / Failed / Cancelled / TimedOut)。cancelled 置位后约 100ms 内返回。
BindUpdate run_bind(const BindOptions& options, const std::atomic<bool>& cancelled,
                    const BindProgress& progress);

std::string build_connect_url(const std::string& base, const std::string& task_id,
                              const std::string& source);

bool decrypt_bind_secret(const std::string& encrypted_base64, const std::string& key_base64,
                         std::string& secret, std::string* error);

} // namespace acecode::im::qqbot
