#pragma once

// LINE Messaging API 的纯逻辑部分(不做 IO):webhook 签名校验、webhook JSON 解析为
// 入站消息、@机器人 识别与去除、回复令牌是否还能用、出站文字分段与每次调用的条数上限。
//
// 平台事实(见 LINE 官方文档,2026-10 核对):
//   - 回调签名 X-Line-Signature = base64(HMAC-SHA256(Channel secret, 原始请求体));
//   - 文字气泡上限 5000 个 UTF-16 单位,不渲染 Markdown;一次 reply / push 最多 5 条;
//   - mention 的 index / length 按 UTF-16 单位计;
//   - reply token 只能用一次,收到后 1 分钟内有效(这里按 50 秒算);重投的事件沿用原令牌,
//     但事件发生 20 分钟后一律失效;
//   - 群里电脑版 LINE 的发言不带 userId,无法识别发言人。

#include "im/transport.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace acecode::im::line {

inline constexpr const char* kPlatform = "line";
inline constexpr std::size_t kMaxTextUnits = 5000;      // LINE 文字气泡上限(UTF-16 单位)
inline constexpr std::size_t kChunkUnits = 4800;        // 分段长度,给“(补发)”等前缀留余量
inline constexpr std::size_t kMaxMessagesPerCall = 5;   // 一次 reply / push 最多 5 个消息对象
inline constexpr std::int64_t kReplyTokenWindowMs = 50 * 1000;
inline constexpr std::int64_t kRedeliveredTokenLimitMs = 20 * 60 * 1000 - 60 * 1000;

// ---- 签名 ----

// 按 LINE 规则计算原始请求体的签名(base64)。channel_secret 为空或摘要失败时返回空串。
std::string sign_body(std::string_view raw_body, const std::string& channel_secret);
// 常量时间比较;签名为空、密钥为空或不一致都返回 false。header 两端空白会被忽略。
bool verify_signature(std::string_view raw_body, std::string_view signature, const std::string& channel_secret);

// ---- webhook 解析 ----

enum class EventKind { Message, Follow, Unfollow, Join, Leave, Unsend, MessageEdited, Postback, Other };

struct WebhookEvent {
    EventKind kind = EventKind::Other;
    std::string type;               // 原始 type
    std::string event_id;           // webhookEventId(去重键,重投时不变)
    bool standby = false;           // mode == "standby":不应发送任何消息
    bool redelivery = false;
    std::int64_t timestamp_ms = 0;  // 事件发生时间
    std::string reply_token;
    std::string source_type;        // user / group / room
    std::string chat_id;            // 私聊 userId;群 groupId;多人聊天 roomId
    std::string user_id;            // 发言人;群里可能为空(电脑版 LINE)
    std::string message_id;
    std::string quoted_message_id;  // 对方引用的消息 id(平台不提供内容)
    bool mentioned_self = false;    // 文字里点名了本机器人
    bool unidentified_sender = false;  // 群消息缺少 userId
    std::optional<Inbound> inbound; // 可交给核心的消息事件
};

struct ParsedWebhook {
    bool valid = false;             // false:不是 LINE webhook 的 JSON 结构
    std::string destination;        // 机器人 userId
    std::vector<WebhookEvent> events;
};

// account = 机器人 userId(Address::account,也用于 mention 的兜底判定);
// received_at_ms = 本机收到 webhook 的墙钟时间,写进 reply_context 用来判断令牌是否过期。
ParsedWebhook parse_webhook(std::string_view body, const std::string& account, std::int64_t received_at_ms);

// 按 UTF-16 单位删除若干 [offset, offset + length) 区间;区间可无序、可重叠,越界部分截掉。
std::string remove_utf16_ranges(const std::string& utf8, std::vector<std::pair<std::size_t, std::size_t>> ranges);
// UTF-16 单位偏移对应的 UTF-8 字节偏移(落在代理对中间时取该字符开头);越界返回 utf8.size()。
std::size_t utf16_to_byte_offset(const std::string& utf8, std::size_t units);

// ---- 回复令牌 ----

struct ReplyToken {
    std::string token;
    std::int64_t received_at_ms = 0;
    std::int64_t event_ts_ms = 0;
    bool redelivery = false;
    std::string quote_token;
};

// 从 Inbound::reply_context 取出回复令牌;没有令牌时返回 nullopt。
std::optional<ReplyToken> reply_token_of(const nlohmann::json& reply_context);
nlohmann::json make_reply_context(const ReplyToken& token, const std::string& message_id);
// 令牌是否还在可用窗口内(不判断是否已经用过)。
bool reply_token_fresh(const ReplyToken& token, std::int64_t now_ms);

// ---- 出站 ----

// Markdown → 纯文本(链接保留为 “文字 (网址)”),再按 kChunkUnits 个 UTF-16 单位分段。
std::vector<std::string> plain_text_chunks(const std::string& markdown);
nlohmann::json text_message(const std::string& text, const std::string& quote_token = {});
nlohmann::json image_message(const std::string& url);
// 按每次调用最多 per_call 条分组。
std::vector<std::vector<nlohmann::json>> batch_messages(const std::vector<nlohmann::json>& messages,
                                                        std::size_t per_call = kMaxMessagesPerCall);

// 429 里的 “You have reached your monthly limit.” 是推送额度用完,不是限频。
bool is_monthly_limit(long status, const std::string& message);
// 回复令牌已失效 / 已用过。
bool is_invalid_reply_token(long status, const std::string& message);

// ---- 杂项 ----

// 宽松取值:对象缺失、字段缺失或类型不符时返回空串 / false,绝不抛异常
// (平台会无通知地增改字段;线程里抛出的异常会直接终止进程)。数字 id 转成十进制字符串。
std::string json_string(const nlohmann::json& object, const char* key);
bool json_bool(const nlohmann::json& object, const char* key);

// RFC 3986 非保留字符之外一律 %XX。
std::string percent_encode(std::string_view value);
// 加好友链接 https://line.me/R/ti/p/<编码后的 basicId>;basic_id 为空时返回空串。
std::string add_friend_url(const std::string& basic_id);
// 打开与官方账号的聊天并预填文字:https://line.me/R/oaMessage/<basicId>/?<文字>(都按 UTF-8 百分号编码)。
// 机主绑定可用它生成二维码,例如预填 "/start 483920";私聊里 "/start <码>" 会解析进 Inbound::start_code。
std::string oa_message_url(const std::string& basic_id, const std::string& text);
// LINE 只能对用户 id(U 开头)显示“正在输入”动画。
bool is_user_id(const std::string& id);
// LINE 图片消息只接受 JPEG / PNG。
bool is_line_image(const std::string& mime_type, const std::string& name);
// 媒体链接里的文件名:只保留 [A-Za-z0-9._-],最长 64 字节,保留扩展名;结果不会以点开头。
std::string safe_file_name(const std::string& name);

} // namespace acecode::im::line
