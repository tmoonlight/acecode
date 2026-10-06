#pragma once

// Discord 机器人协议常量与纯解析逻辑(不做 IO)。
// 参照:Discord 官方文档(discord-api-docs,2026-09)与 hermes-agent 的 Discord 适配器。
//
// 地址约定(与核心一致):
//   - account = 机器人用户 id(READY.user.id,同 GET /users/@me 的 id);
//   - 私聊:kind=Private,chat = sender = 对方用户 id;私聊频道 id 只放在 reply_context;
//   - 服务器频道:kind=Group,chat = 频道 id(在子区里就是子区自己的频道 id),sender = 发言人 id。

#include "im/transport.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace acecode::im::discord {

inline constexpr const char* kPlatform = "discord";
inline constexpr const char* kApiBase = "https://discord.com/api/v10";
inline constexpr const char* kPortalUrl = "https://discord.com/developers/applications";
inline constexpr const char* kProjectUrl = "https://github.com/tmoonlight/acecode";

// GUILDS(1) | GUILD_MESSAGES(1<<9) | DIRECT_MESSAGES(1<<12) | MESSAGE_CONTENT(1<<15)
inline constexpr int kIntents = 37377;

// 单条消息上限 2000 字符,官方没说按什么单位计;按 UTF-16 单位(不小于码点数)切到 1990 最稳妥。
inline constexpr std::size_t kMaxTextUnits = 1990;

// 邀请链接里的机器人权限:查看频道(1<<10) + 发送消息(1<<11) + 附加文件(1<<15)
// + 读取消息历史(1<<16) + 在子区发送消息(1<<38)。
inline constexpr std::uint64_t kInvitePermissions = 274878008320ULL;

// GET /applications/@me 的 flags:已批准(>=100 个服务器)/ 开发者后台开关(<100 个服务器)。
inline constexpr std::int64_t kAppFlagMessageContent = std::int64_t{1} << 18;
inline constexpr std::int64_t kAppFlagMessageContentLimited = std::int64_t{1} << 19;

// 消息 flags:语音消息。
inline constexpr std::int64_t kMessageFlagVoice = std::int64_t{1} << 13;

// "DiscordBot (https://github.com/tmoonlight/acecode, <版本>)"。没有合法 UA 时 Cloudflare 可能拦截请求。
std::string discord_user_agent();

// 机器人邀请链接(scope=bot,权限见 kInvitePermissions);application_id 为空时返回空串。
std::string invite_url(const std::string& application_id);

// 网关地址 → 实际连接地址:去掉末尾 '/' 与已有查询串,再拼 "/?v=10&encoding=json"(不压缩)。
std::string gateway_connect_url(const std::string& base);

enum class IntentState { Unknown, Enabled, Disabled };

// 由应用对象的 flags 判断 Message Content Intent 是否已开启;没有 flags 时为 Unknown。
IntentState message_content_intent(const nlohmann::json& application);

struct ApiError {
    long status = 0;          // HTTP 状态码;0 = 没拿到响应
    long code = 0;            // Discord JSON 错误码(10008、50035…)
    std::string message;      // 平台原文(英文)
    std::string detail;       // errors 里逐字段的原因,形如 "message_reference: Unknown message"
    double retry_after = 0;   // 429:需要等待的秒数(小数)
    bool global = false;      // 429:是否全局限流
};

// 解析失败响应 {"code","message","errors","retry_after","global"};无法解析时用 HTTP 状态码兜底。
ApiError parse_api_error(long status, const std::string& body);

// 回复定位被拒(被回复的消息不存在、是系统消息、没有读取历史权限):去掉 message_reference 重发即可。
bool reference_rejected(const ApiError& error);
// 文件过大(413 / 40005):改发文字说明。
bool upload_too_large(const ApiError& error);
// 给用户看的一句话中文原因(不含凭据)。
std::string describe_error(const ApiError& error);

struct ParsedMessage {
    std::optional<Inbound> inbound;
    std::string drop_reason;  // inbound 为空时的原因(英文,只写日志)
    std::string dm_user;      // 私聊:对方用户 id 与私聊频道 id,供发送时复用
    std::string dm_channel;
};

// 解析网关 MESSAGE_CREATE 的 d。bot_id 为机器人用户 id;bot_role_ids 为机器人的托管角色
// (有人 @ 了机器人的角色也算点名)。过滤:自己、其他机器人、webhook、系统消息、群组私聊、
// 非普通/回复类型、去掉 @ 后既没文字也没附件。
ParsedMessage parse_message_create(const nlohmann::json& d, const std::string& bot_id,
                                   const std::set<std::string>& bot_role_ids);

// GUILD_CREATE 里属于本机器人的托管角色 id(roles[].tags.bot_id == bot_id)。
std::vector<std::string> bot_role_ids_in_guild(const nlohmann::json& guild, const std::string& bot_id);

// 处理正文里的提及:去掉 @机器人(<@id>、<@!id>)与机器人托管角色(<@&id>),其他用户的提及
// 按 mentions[] 换成 "@名称";首尾去空白。mentioned 非空时写入是否出现了对机器人的提及。
std::string render_content(const std::string& content, const std::string& bot_id,
                           const std::set<std::string>& bot_role_ids, const nlohmann::json& mentions,
                           bool* mentioned = nullptr);

// Discord 原生支持大部分 Markdown;不支持的两处在这里处理:GFM 表格整体包进代码块(等宽显示),
// 四级及以下标题改成粗体。代码块里的内容原样保留。
std::string format_markdown(const std::string& markdown);

// URL 的主机名(小写,不含端口与用户信息);解析不出时返回空串。
std::string url_host(const std::string& url);
// CDN 附件地址 .../attachments/<频道>/<附件 id>/<文件名> 里的附件 id;不是附件地址时返回空串。
std::string attachment_id_from_url(const std::string& url);

} // namespace acecode::im::discord
