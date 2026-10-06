#pragma once

// 微信 iLink 机器人(微信 ClawBot)协议的纯逻辑部分:不做 IO,便于单测。
//
// 协议要点(详见 openspec add-desktop-im-channels 的微信协议调研):
//   - 扫码登录得到一个独立的机器人身份 xxx@im.bot(bot_token + ilink_bot_id),只有扫码的
//     那个微信用户能和它私聊;没有群聊。
//   - 收消息靠 POST ilink/bot/getupdates 长轮询,游标 get_updates_buf 原样回传、必须持久化。
//   - 回复要带对方最近一条消息里的 context_token。
//   - 应用层错误以 HTTP 200 + {"ret"/"errcode" 非 0} 返回,字段可能缺失(缺失按 0)。
//   - 媒体走单独 CDN,内容用一次性 16 字节密钥做 AES-128-ECB + PKCS#7。
// 版本按 hermes-agent 验证过的 2.2.0 声明(channel_version 与 iLink-App-ClientVersion 一致)。

#include "im/transport.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace acecode::im::weixin {

inline constexpr const char* kPlatform = "weixin";
inline constexpr const char* kApiBase = "https://ilinkai.weixin.qq.com";
inline constexpr const char* kCdnBase = "https://novac2c.cdn.weixin.qq.com/c2c";
inline constexpr const char* kChannelVersion = "2.2.0";
inline constexpr const char* kAppId = "bot";
inline constexpr const char* kBotType = "3";

inline constexpr const char* kEpGetBotQr = "ilink/bot/get_bot_qrcode";
inline constexpr const char* kEpGetQrStatus = "ilink/bot/get_qrcode_status";
inline constexpr const char* kEpGetUpdates = "ilink/bot/getupdates";
inline constexpr const char* kEpSendMessage = "ilink/bot/sendmessage";
inline constexpr const char* kEpSendTyping = "ilink/bot/sendtyping";
inline constexpr const char* kEpGetConfig = "ilink/bot/getconfig";
inline constexpr const char* kEpGetUploadUrl = "ilink/bot/getuploadurl";

// 消息条目类型(MessageItem.type)与消息来源(WeixinMessage.message_type)。
inline constexpr int kItemText = 1;
inline constexpr int kItemImage = 2;
inline constexpr int kItemVoice = 3;
inline constexpr int kItemFile = 4;
inline constexpr int kItemVideo = 5;
inline constexpr int kMessageTypeBot = 2;

// 给用户看的“登录失效”说明;传输层状态与发送失败共用。
inline constexpr const char* kSessionExpiredText = "微信登录已失效,请在设置页重新扫码";

// "2.2.0" → ((2<<16)|(2<<8)|0) = 131584;无法解析的部分按 0。
std::uint32_t client_version_number(const std::string& version);
// X-WECHAT-UIN:随机 uint32 的十进制字符串再做 base64(不是真实 UIN,每次请求换一个)。
std::string wechat_uin(std::uint32_t value);

// base 与相对路径之间只保留一个 '/'。
std::string join_url(const std::string& base, const std::string& path);
// 除 [A-Za-z0-9-_.~] 外全部百分号编码(等同 Python quote(safe=''))。
std::string url_encode(const std::string& value);

// ---- 响应包络 ----

struct Envelope {
    bool parsed = false;   // 是 JSON 对象
    int ret = 0;           // 缺失按 0
    int errcode = 0;       // 缺失按 0
    std::string errmsg;
    nlohmann::json body = nlohmann::json::object();
};

Envelope parse_envelope(const std::string& body);
bool envelope_failed(const Envelope& envelope);
// -14,或 -2 且 errmsg 为 "unknown error"(hermes #17228):机器人 token 已失效。
bool is_session_expired(int ret, int errcode, const std::string& errmsg);
// -2 且不是上面的失效情况、也不是参数错误:平台限频。
bool is_rate_limited(int ret, int errcode, const std::string& errmsg);

// uint64 / int64 / 字符串形式的 id 一律转成十进制字符串;其它类型返回空串。
std::string id_string(const nlohmann::json& value);
// 平台 id(o9cq…@im.wechat、e06c…@im.bot)只允许常见字符,长度 1..128。
bool valid_id(const std::string& value);

// ---- 媒体 ----

// 入站附件的下载信息,序列化后放进 Attachment::remote_ref。
struct MediaRef {
    std::string query_param;  // CDNMedia.encrypt_query_param
    std::string full_url;     // CDNMedia.full_url
    std::string aes_key;      // 16 字节原始密钥;空 = 明文
    bool key_invalid = false; // 平台给了密钥但格式无法识别
    std::string kind;         // image / voice / file / video
};

std::string encode_media_ref(const MediaRef& ref);
std::optional<MediaRef> decode_media_ref(const std::string& text);

std::optional<std::string> hex_decode(const std::string& hex);
// CDNMedia.aes_key:base64 解码后 16 字节即原始密钥;32 个十六进制字符则再按十六进制解码。
std::optional<std::string> parse_aes_key(const std::string& value);
// 出站条目里的 aes_key = base64(32 个小写十六进制字符),不是 base64(原始 16 字节)——
// 后者会让对方看到灰色方块(hermes 实测)。
std::string outbound_aes_key(const std::string& raw_key);
// 宽松去 PKCS#7:末字节 p 在 1..16 且末 p 字节都等于 p 才去掉,否则原样返回。
std::string strip_pkcs7_lenient(std::string data);

enum class UploadMediaType { Image = 1, Video = 2, File = 3 };

std::string guess_mime(const std::string& file_name);
// image/*(SVG 除外)→ 图片;video/* → 视频;其余(包括音频)一律按文件发送。
UploadMediaType upload_media_type(const std::string& mime_type, const std::string& file_name);

std::string cdn_download_url(const std::string& cdn_base, const std::string& query_param);
std::string cdn_upload_url(const std::string& cdn_base, const std::string& upload_param,
                           const std::string& filekey);
// full_url 只允许微信自家域名(或配置的 CDN 地址),防止被诱导去请求任意内网地址。
bool media_host_allowed(const std::string& url, const std::string& cdn_base);

// ---- 入站解析 ----

struct ParsedMessage {
    std::optional<Inbound> inbound;  // 被丢弃时为空
    std::string sender;              // from_user_id
    std::string message_id;
    std::string context_token;
    std::int64_t create_time_ms = 0;
    bool from_user = false;          // 真实用户发来的(不是机器人自己 / BOT 类型)
    std::string drop_reason;         // 英文,只用于日志
};

// account = 机器人 id(ilink_bot_id)。私聊:chat = sender = from_user_id,mentioned = true。
ParsedMessage parse_message(const nlohmann::json& message, const std::string& account);

// ---- 出站 ----

nlohmann::json text_item(const std::string& text);
nlohmann::json media_item(UploadMediaType type, const std::string& encrypt_query_param,
                          const std::string& raw_key, std::uint64_t ciphertext_size,
                          std::uint64_t plaintext_size, const std::string& md5_hex,
                          const std::string& file_name);
// sendmessage 的请求体(base_info 由 Api 统一补上)。context_token 为空时不带该字段。
nlohmann::json build_send_body(const std::string& to, const std::string& client_id,
                               const nlohmann::json& item, const std::string& context_token);

// ---- 入站整理 ----

// iLink 会把超过约 2048 字的用户消息拆成几条依次投递。长片段(>= split_threshold 个字符)
// 先暂存,等待 wait 内的后续片段;后续片段不长时立即合并交付,超时则原样交付。
// 普通长度的消息不等待,零延迟。
struct MergeLimits {
    std::size_t split_threshold = 1800;  // 码点数;真实拆分点约 2048,留出余量
    std::chrono::milliseconds wait{5000};
};

class FragmentMerger {
public:
    using Clock = std::chrono::steady_clock;
    explicit FragmentMerger(MergeLimits limits = {});

    // 新到一条消息,返回现在应交给上层的消息(保持同一发送人的先后顺序)。
    std::vector<Inbound> push(Inbound inbound, Clock::time_point now);
    // 等待已超时的暂存消息。
    std::vector<Inbound> take_due(Clock::time_point now);
    std::vector<Inbound> take_all();
    bool empty() const { return pending_.empty(); }

private:
    struct Pending {
        Inbound inbound;
        Clock::time_point deadline;
    };
    MergeLimits limits_;
    std::map<std::string, Pending> pending_;  // 键:发送人
};

// 入站去重:按消息 id;另外平台偶尔会换一个 message_id 重发同一条内容(hermes #16182),
// 同一发送人、同一文字、同一发送时间(create_time_ms)的只处理一次。没有发送时间时,
// 只在 content_window 内按文字去重(窗口很短,避免吞掉用户有意重复的“继续”)。
class InboundDeduper {
public:
    using Clock = std::chrono::steady_clock;
    explicit InboundDeduper(std::size_t capacity = 1000,
                            std::chrono::milliseconds content_window = std::chrono::seconds(10));

    // 返回 true 表示重复。message_id 为空时只做内容判定。
    bool seen_id(const std::string& message_id);
    bool seen_content(const std::string& sender, const std::string& text, std::int64_t create_time_ms,
                      Clock::time_point now);

private:
    bool remember(const std::string& key);

    std::size_t capacity_;
    std::chrono::milliseconds window_;
    std::set<std::string> keys_;
    std::deque<std::string> order_;
    std::map<std::string, Clock::time_point> recent_;  // 无发送时间的内容键 → 最近一次
};

} // namespace acecode::im::weixin
