#pragma once

// QQ 官方机器人(开放平台 API v2)协议常量与纯解析逻辑。
// 参照:QQ 开放平台文档,以及 WorkBuddy 5.6.2 内置 QQ 插件与 Hermes qqbot 适配器
// (add-desktop-im-channels design D9/D10)。

#include "im/transport.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace acecode::im::qqbot {

// 2026-08 起官方文档统一使用 api.bot.qq.com;旧域名 api.sgroup.qq.com / bots.qq.com
// (WorkBuddy 5.6.2 仍在用)同样可用,作为备选不默认启用。
inline constexpr const char* kApiBase = "https://api.bot.qq.com";
inline constexpr const char* kTokenUrl = "https://api.bot.qq.com/app/getAppAccessToken";
inline constexpr const char* kPortalBase = "https://q.qq.com";
inline constexpr const char* kBindSource = "acecode";

// 只订阅单聊与群 @ 消息(GROUP_AND_C2C_EVENT);频道消息不接。
inline constexpr int kIntents = 1 << 25;

inline constexpr int kMsgTypeText = 0;
inline constexpr int kMsgTypeMarkdown = 2;
inline constexpr int kMsgTypeMedia = 7;

inline constexpr int kFileTypeImage = 1;
inline constexpr int kFileTypeVideo = 2;
inline constexpr int kFileTypeFile = 4;

// 单条文本上限(按字符);WorkBuddy 用 5000,Hermes 用 4000,取保守值。
inline constexpr std::size_t kMaxTextChars = 4000;

enum class CloseAction {
    Resume,        // 普通断线:带 session 恢复
    Identify,      // 会话已失效:重新登录
    RefreshToken,  // 令牌失效:刷新令牌后恢复
    RateLimited,   // 触发网关频控:至少等 60 秒
    Fatal,         // 机器人被封禁 / 不在线 / 仅限沙箱:停止重连
};

struct CloseDecision {
    CloseAction action = CloseAction::Resume;
    std::string reason;  // 给用户看的中文原因
};

CloseDecision classify_close(int code);

// 把网关派发事件(op 0)转换为入站消息。只处理单聊与群消息;
// 其它事件、缺字段、空消息返回 nullopt。now_ms 记入回复上下文,用于被动回复窗口计时。
std::optional<Inbound> parse_message_event(const std::string& type, const nlohmann::json& d,
                                           const std::string& app_id, std::int64_t now_ms);

struct ApiError {
    long status = 0;
    long code = 0;
    std::string message;
};

// 解析 OpenAPI 失败响应({"code":..,"message":..});无法解析时用 HTTP 状态码兜底。
ApiError parse_api_error(long status, const std::string& body);

// 上传富媒体时的 file_type:图片 1、视频 2,其余一律按普通文件 4 发送。
int file_type_for(const std::string& mime_type, const std::string& name);

// 附件地址可能以 "//" 开头(省略协议),补成 https。
std::string normalize_attachment_url(const std::string& url);

} // namespace acecode::im::qqbot
