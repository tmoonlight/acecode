#pragma once

// 飞书 / Lark 机器人协议常量与纯逻辑(不做 IO):接入地址、长连接配置与握手分类、
// OpenAPI 响应解析与错误分类、im.message.receive_v1 事件解析(@ 判定与文本清理)、
// 出站 Markdown → post(md 节点)与分段、文件上传路由。
// 协议细节见 add-desktop-im-channels 的飞书协议调研(长连接 pbbp2 帧、事件 2.0)。

#include "im/transport.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acecode::im::feishu {

inline constexpr const char* kPlatform = "feishu";
inline constexpr const char* kDomainFeishu = "feishu";
inline constexpr const char* kDomainLark = "lark";
inline constexpr const char* kFeishuBase = "https://open.feishu.cn";
inline constexpr const char* kLarkBase = "https://open.larksuite.com";

// "lark"(大小写不敏感)→ open.larksuite.com;其它一律 open.feishu.cn。
// 飞书与 Lark 是两套独立的云,选错时换令牌会报 10014 / 1000040345。
std::string base_for_domain(const std::string& domain);

// 单条文本分段上限(字符,官方渠道 SDK 用 3500)与单条消息 content 序列化后的字节上限
// (post 请求体上限 30 KB,留 2 KB 余量给外层 JSON 转义)。
inline constexpr std::size_t kMaxTextChars = 3500;
inline constexpr std::size_t kMaxContentBytes = 28u * 1024u;
inline constexpr std::uint64_t kMaxImageBytes = 10u * 1024u * 1024u;
inline constexpr std::uint64_t kMaxFileBytes = 30u * 1024u * 1024u;
// 平台失败重投最长到 6 小时;创建时间超过 30 分钟的消息视为陈旧重放,直接丢弃。
inline constexpr std::int64_t kStaleEventMs = 30LL * 60LL * 1000LL;

// ---------------------------------------------------------------- 长连接

// 服务端下发的长连接参数(单位秒),以服务端为准;本地默认值与官方 SDK 一致。
struct ClientConfig {
    int reconnect_count = -1;       // -1 = 无限重连
    int reconnect_interval_s = 120;
    int reconnect_nonce_s = 30;     // 首次重连前的随机抖动上限
    int ping_interval_s = 120;
};

// 用 raw 里出现的字段覆盖 config(ReconnectCount / ReconnectInterval / ReconnectNonce /
// PingInterval);负值的间隔与 0 的心跳间隔忽略。返回是否应用了至少一个字段。
bool apply_client_config(const nlohmann::json& raw, ClientConfig* config);

struct EndpointInfo {
    bool ok = false;
    bool fatal = false;        // 客户端错误(凭据无效等):不再重连
    long status = 0;           // HTTP 状态码;0 = 没拿到响应
    std::int64_t code = 0;
    std::string message;       // 给用户看的中文原因;不含地址与密钥
    std::string url;           // wss 地址,带票据,绝不能写日志
    nlohmann::json client_config;  // 原始 ClientConfig;没有时为 null
    std::string device_id;     // 连接 id,只用于日志
    std::int32_t service_id = 0;
};

// 解析 POST /callback/ws/endpoint 的响应。HTTP 非 200、code 1 / 1000040343、地址为空
// 都可重试;其它非 0 code 是客户端错误(致命)。
EndpointInfo parse_endpoint_response(long status, const std::string& body);

// 取 URL 查询参数(%XX 解码);不存在返回空串。
std::string url_query_param(const std::string& url, const std::string& name);

struct HandshakeDecision {
    bool fatal = false;
    std::string message;
};

// WebSocket 升级失败的分类。handshake_status / auth_errcode 来自响应头 Handshake-Status /
// Handshake-Autherrcode(拿不到时传 0):403 → 致命;514 且 1000040350(连接数超限)→ 致命;
// 其它一律可重试。
HandshakeDecision classify_handshake(long http_status, int handshake_status, std::int64_t auth_errcode);

// ---------------------------------------------------------------- OpenAPI

struct ApiResult {
    bool ok = false;
    long status = 0;            // HTTP 状态码;0 = 没拿到响应
    std::int64_t code = 0;      // 飞书业务错误码
    std::string msg;            // 平台原文
    std::string error;          // 传输层错误(status == 0 时),已脱敏
    nlohmann::json body = nlohmann::json::object();
    nlohmann::json data = nlohmann::json::object();  // body.data(没有时为空对象)
    bool cancelled = false;
    bool auth_failed = false;   // 换令牌时凭据被拒;error 为给用户看的原因
    std::chrono::seconds retry_after{0};  // 限流响应头 x-ogw-ratelimit-reset(秒)
    std::string log_id;         // 响应头 X-Tt-Logid,排障时飞书支持会要;只写日志
};

// 飞书的错误常带 HTTP 400/401/429 且仍有 JSON 体:一律解析响应体。ok = 2xx 且 code == 0。
ApiResult parse_api_response(long status, const std::string& body);

// 令牌失效类错误码(99991661 / 99991663 / 99991664 / 99991665):刷新令牌后重试一次。
bool is_token_invalid(std::int64_t code);
// 限流:HTTP 429,或 99991400 / 99991402 / 11020 / 11021 / 230020。
bool is_rate_limited(const ApiResult& result);
// 解析 x-ogw-ratelimit-reset(需要等待的秒数);非法或缺失返回 0,上限 1 小时。
std::chrono::seconds parse_retry_after(const std::string& value);
// 被限流后这次重发前的等待:本地退避与平台要求(封顶 60 秒)取大。
std::chrono::milliseconds rate_limit_wait(const ApiResult& result, std::chrono::milliseconds fallback);
// 值得原样重试的临时错误:网络错误、HTTP 5xx、code 落在 50000–59999。限流单独处理。
bool is_transient(const ApiResult& result);
// post 内容被拒(230001 或 msg 含 "content format of the post type is incorrect"):改发纯文本。
bool is_post_rejected(const ApiResult& result);
// 回复目标已撤回 / 不存在(230011 / 231003):改为直接发到会话。
bool is_reply_target_gone(std::int64_t code);

// 给用户看的一句话中文原因(不含凭据)。
std::string describe_error(const ApiResult& result);
// 换取 tenant_access_token 失败(code != 0)时给用户看的原因。
std::string describe_token_error(std::int64_t code, const std::string& msg);

struct BotInfo {
    bool ok = false;
    std::string open_id;    // 机器人自己的 open_id,只用于判定群里是否 @ 了机器人
    std::string name;
    int activate_status = -1;  // 2 = 已启用(唯一正常状态)
    bool ready() const { return ok && activate_status == 2; }
};

// 解析 GET /open-apis/bot/v3/info(bot 在顶层,也兼容 data.bot)。
BotInfo parse_bot_info(const nlohmann::json& body);
// activate_status 不为 2 时给用户的提示。
std::string describe_activate_status(int activate_status);

// ---------------------------------------------------------------- 入站事件

struct BotIdentity {
    std::string open_id;
    std::string name;
};

struct Mention {
    std::string key;            // "@_user_1"
    std::string open_id;
    std::string user_id;
    std::string name;
    std::string mentioned_type; // "user" / "bot"
};

std::vector<Mention> parse_mentions(const nlohmann::json& mentions);

// 是否点名了机器人。优先按 open_id 比对(权威);还没拿到机器人身份时退化为按名字、
// 再退化为 mentioned_type == "bot"。@所有人 不算(与官方渠道 SDK 默认一致)。
bool mentions_bot(const std::vector<Mention>& mentions, const BotIdentity& bot);

// 把正文里的 @ 占位符换成可读文本:机器人自己的 @ 在开头 / 结尾处去掉、在中间保留为
// "@名字";其他人 → "@名字";未知占位符 → 空格;@_all → "@all"。CRLF → LF,首尾空白去掉,
// 每行行尾空白去掉;行内缩进保留(用户贴的代码不被破坏)。
std::string clean_text(const std::string& raw, const std::vector<Mention>& mentions, const BotIdentity& bot);

struct ExtractedContent {
    std::string text;  // 仍含 @_user_N 占位符,交给 clean_text 处理
    std::vector<Attachment> attachments;
};

// 按 message_type 从已解析的 content 里取文字与附件(post 的 img / media 节点也算附件)。
ExtractedContent extract_content(const std::string& message_type, const nlohmann::json& content,
                                 const std::string& message_id);

// 附件下载定位:Attachment::remote_ref 存成 JSON {"message_id","key","type"},type 为 image / file。
struct ResourceRef {
    std::string message_id;
    std::string key;
    std::string type;
};
std::string make_resource_ref(const std::string& message_id, const std::string& key, const std::string& type);
std::optional<ResourceRef> parse_resource_ref(const std::string& remote_ref);

enum class EventDisposition {
    Message,   // inbound 有值
    Ignored,   // 不是 im.message.receive_v1(或 1.0 事件)
    Stale,     // 创建时间超过 30 分钟(平台重投的旧消息)
    FromBot,   // 机器人 / 应用发的,或机器人自己的回声
    Empty,     // 去掉 @机器人 后既没有文字也没有附件
    Invalid,   // JSON 损坏或缺关键字段
};

struct ParsedEvent {
    EventDisposition disposition = EventDisposition::Ignored;
    std::string event_type;
    std::string message_id;      // 已解析出时填写(用于日志与去重)
    std::optional<Inbound> inbound;
};

// 解析一个 type=event 帧的完整 payload。account = App ID;now_epoch_ms 用于陈旧判断。
// 私聊:chat = sender = 对方 open_id;群聊:chat = chat_id,sender = 发言人 open_id。
// reply_context = {message_id, chat_id, chat_type[, thread_id]}。
ParsedEvent parse_event(const std::string& payload, const BotIdentity& bot, const std::string& account,
                        std::int64_t now_epoch_ms);

// ---------------------------------------------------------------- 出站

// 是否按 Markdown 发送(整条消息判定一次,所有分段沿用,避免纯文字分段把 ** 原样露出)。
// 规则对照 hermes 的 _MARKDOWN_HINT_RE:表格、标题、列表、分隔线、代码、粗/斜/删除线、
// <u>、链接、引用。手写扫描,不用 std::regex。
bool looks_like_markdown(std::string_view text);

// post 内容 {"zh_cn":{"content":rows}}(不能再包一层 "post");每个 ``` 代码块单独占一行
// md 节点,前后的正文各占一行 —— 飞书的 md 渲染器遇到大段内嵌代码块会吞掉后面的内容。
nlohmann::json build_post_content(const std::string& markdown);
nlohmann::json build_text_content(const std::string& text);

// 序列化 JSON;非法 UTF-8 用替换字符兜底,绝不抛异常。
std::string dump_json(const nlohmann::json& value);

// 出站分段:先按字符上限切(代码块跨段时自动补闭合 / 重开围栏),再确保每段的 content
// 序列化后不超过 max_bytes(超出就把该段按一半上限再切)。
std::vector<std::string> split_outbound(const std::string& text, bool markdown,
                                        std::size_t max_chars = kMaxTextChars,
                                        std::size_t max_bytes = kMaxContentBytes);

// 出站目标:有 reply_context.message_id 时首段走回复接口;有 chat_id 时一律按 chat_id 发
// (私聊也有 chat_id);否则群 → chat_id = address.chat,私聊 → open_id = address.chat。
struct SendTarget {
    std::string receive_id_type;  // chat_id / open_id
    std::string receive_id;
    std::string reply_to;         // 触发本回合的消息 id(可空)
    bool in_thread = false;       // 话题内:所有分段都走回复,留在同一话题
};
SendTarget send_target(const Address& to, const nlohmann::json& reply_context);

// 文件上传路由。image = true 时走图片接口(≤ 10 MB 的常见图片),以 msg_type=image 发送;
// 否则走文件接口:.opus → opus / audio,.mp4 → mp4 / media,pdf / doc / xls / ppt 各自
// 类型 / file,其余 stream / file。上传类型必须与 msg_type 一致(否则 230055)。
struct FileRoute {
    bool image = false;
    std::string file_type;
    std::string msg_type;
};
FileRoute route_file(const std::string& name, const std::string& mime_type, std::uint64_t size);

} // namespace acecode::im::feishu
