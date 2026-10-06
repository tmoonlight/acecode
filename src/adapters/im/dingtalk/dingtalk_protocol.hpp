#pragma once

// 钉钉企业内部应用机器人(Stream 模式)协议常量与纯解析逻辑,不做 IO。
// 参照:open-dingtalk 官方 Stream SDK(python / node / go)、aliyun/dingtalk-sdk 的机器人 OpenAPI
// 字段名、钉钉官方 openclaw connector,以及 Hermes 钉钉适配器的踩坑记录。

#include "im/transport.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acecode::im::dingtalk {

inline constexpr const char* kPlatform = "dingtalk";
// 新版 OpenAPI(令牌、Stream 注册、机器人发消息、文件下载)。
inline constexpr const char* kApiBase = "https://api.dingtalk.com";
// 旧版 OAPI(媒体上传、会话 webhook)。新版令牌在这里同样有效。
inline constexpr const char* kOapiBase = "https://oapi.dingtalk.com";
// 机器人收消息的回调主题。
inline constexpr const char* kBotMessageTopic = "/v1.0/im/bot/messages/get";

// 单条消息上限(按码点)。社区资料给出 4000 / 5000 / 20000 三种说法,取保守值。
inline constexpr std::size_t kMaxTextChars = 3500;
// Markdown 消息的 title 只用于通知栏与会话列表预览,截到 20 个字符。
inline constexpr std::size_t kTitleChars = 20;
inline constexpr const char* kDefaultTitle = "ACECode";

// 消息里没有 senderStaffId(组织外用户)时,发言人用加密的 senderId,形如 "$:LWCP_v1:$…"。
// 这种 id 不能用于机器人单聊 OpenAPI,只能经会话 webhook 回复。
bool is_encrypted_sender_id(const std::string& id);

// 把 Stream 推送的机器人消息(data 二次解析后的对象)转换为入站消息。
//   - conversationType "1" = 单聊:chat = sender = senderStaffId(没有时用 senderId);
//   - "2" = 群聊:chat = conversationId,sender = senderStaffId(没有时用 senderId);
//   - message_id 取 msgId(重投时不变);缺失时用 fallback_id(Stream 帧头的 messageId);
//   - 群聊 mentioned 看 isInAtList / atUsers 是否含机器人自己;两者都缺时视为点名
//     (企业内部机器人在群里本来只能收到 @ 它的消息);
//   - 文本、富文本、图片、文件、语音(带识别文字)、视频、文档卡片都转成文字或附件,
//     附件的 remote_ref 是 downloadCode;无文字也无附件时返回 nullopt。
// 地址字段只做长度与可打印字符检查,不调用 Address::valid()。
std::optional<Inbound> parse_robot_message(const nlohmann::json& data, const std::string& client_id,
                                           const std::string& fallback_id = {});

// 从引用消息、富文本等任意形态的 content 里抽出可读文字(递归最多 3 层)。
std::string extract_text(const std::string& msgtype, const nlohmann::json& content, int depth = 0);

// 新版 OpenAPI 失败形如 HTTP 4xx + {"code","message","requestid"};
// 旧版 OAPI 与会话 webhook 一律 HTTP 200 + {"errcode","errmsg"}。两种都解析到这里。
struct ApiError {
    long status = 0;          // HTTP 状态码;0 = 没拿到响应
    std::string code;         // 新版 OpenAPI 的字符串错误码
    long errcode = 0;         // OAPI / webhook 的数字错误码
    std::string message;      // 平台给的原因(已去掉凭据前由调用方脱敏)
    std::string request_id;   // 只写日志
};

// 解析失败响应;body 为空或不是 JSON 时用 HTTP 状态码兜底。
ApiError parse_api_error(long status, const std::string& body);
// HTTP 200 的响应体里是否带了非零 errcode(OAPI 与 webhook 的失败形态)。
bool body_has_errcode(const std::string& body, ApiError* error);

// 令牌失效(InvalidAuthentication / HTTP 401 / errcode 40014):清掉缓存令牌后重试一次。
bool is_token_error(const ApiError& error);

enum class Throttle {
    None,
    Qps,   // 接口 QPS 超限(code 含 QpsLimit):短暂等待后重试
    Rate,  // 发送过快(send.too.fast / 130101 / 90018 / 90006 / HTTP 429):长等待后重试
};
Throttle throttle_kind(const ApiError& error);

// 会话 webhook 已失效(errcode 300001 "session 不存在" 等),应改走 OpenAPI。
bool is_webhook_gone(const ApiError& error);

// 给用户看的一句中文原因(权限未开、IP 白名单、网络不通等给出处理办法);不含凭据,
// 调用方仍要再经 redact_secrets。
std::string describe_api_error(const ApiError& error);

// 防 SSRF:只接受 https://api.dingtalk.com/ 或 https://oapi.dingtalk.com/ 开头的 webhook,
// 以及 extra_bases 里列出的前缀(测试把 OpenAPI 地址换成本机假服务时使用)。
bool webhook_url_allowed(const std::string& url, const std::vector<std::string>& extra_bases);

// Markdown 消息的标题:第一行非空文字去掉 # * > - 等标记与首尾空白,截到 kTitleChars 个码点;
// 为空时用 fallback。
std::string markdown_title(std::string_view markdown, const std::string& fallback = kDefaultTitle);

// 钉钉 Markdown 渲染器的两个怪癖(Hermes 实测):
//   - 编号列表行的上一行是普通文字(非空、也不是编号行)时,中间要补一个空行,否则不渲染成列表;
//   - 带缩进的 ``` 围栏不被识别,去掉围栏行(及其内容)前的公共缩进。
std::string normalize_markdown(std::string_view markdown);

// 媒体上传的 type:image / voice / file(视频也按 file 上传,与官方 connector 一致)。
std::string upload_type_for(const std::string& mime_type, const std::string& name);
bool is_image_file(const std::string& mime_type, const std::string& name);
// 文件扩展名(小写、不带点);没有扩展名时为空。
std::string file_extension(const std::string& name);

// URL 查询参数编码(RFC 3986 非保留字符原样保留)。
std::string url_encode(const std::string& value);

} // namespace acecode::im::dingtalk
