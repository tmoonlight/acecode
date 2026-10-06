#pragma once

// IM 平台传输层的统一接口(add-desktop-im-channels D3)。
//
// 传输层只负责“和某个 IM 平台说话”:连接、收消息、发文字/文件、下载附件。
// 它不认识 ACECode 会话;会话绑定、命令、授权都在 host/channels/core。
// 平台差异(QQ 被动回复额度、Telegram 限速与 HTML 格式)全部收在各自实现内部,
// 核心只看 Capabilities 决定如何分段与是否按回合合并。

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace acecode::im {

enum class ChatKind { Private, Group };

// 一个 IM 会话的位置。群聊按“群 + 发言人”区分(QQ 的群成员身份按群隔离)。
struct Address {
    std::string platform;  // "qq" / "telegram"
    std::string account;   // QQ AppID / Telegram 机器人 id
    ChatKind kind = ChatKind::Private;
    std::string chat;      // 私聊:对方 id;群聊:群 id
    std::string sender;    // 发言人 id(私聊时与 chat 相同)
    std::string thread;    // Telegram 话题 id,仅用于回复定位,不参与会话键

    // 结构化会话键(JSON 数组字符串),用于绑定、回执和授权记录。
    std::string key() const;
    nlohmann::json to_json() const;
    static Address from_json(const nlohmann::json& value);
    bool valid() const;
};

enum class AttachmentKind { Image, File, Voice, Video, Sticker, Other };

struct Attachment {
    AttachmentKind kind = AttachmentKind::Other;
    std::string name;
    std::string mime_type;
    std::uint64_t size = 0;    // 0 = 平台没有给出
    std::string remote_ref;    // QQ 附件 url / Telegram file_id
    std::string transcript;    // 平台给出的语音识别文字(QQ asr_refer_text)
};

struct Inbound {
    Address address;
    std::string message_id;
    std::string text;           // 已去掉 @机器人 的前缀
    bool mentioned = false;     // 群聊里是否点名机器人;私聊恒为 true
    std::string quote_text;     // 被引用消息的文字(可空)
    std::string sender_name;    // 展示名(可空),只用于设置页的待批准请求
    std::string start_code;     // Telegram `/start <码>` 的参数(机主绑定码)
    std::vector<Attachment> attachments;
    nlohmann::json reply_context = nlohmann::json::object();  // 平台私有的回复定位信息
};

enum class LinkState { Stopped, Connecting, Connected, Retrying, Failed };

const char* link_state_name(LinkState state);

struct TransportStatus {
    LinkState state = LinkState::Stopped;
    std::string detail;          // 给用户看的原因;绝不含凭据
    bool retry_stopped = false;  // 致命错误后不再自动重试,等用户处理
    std::string account;         // 平台账号 id(QQ AppID / Telegram 机器人 id)
    std::string display_name;    // 机器人名称(可空)
    nlohmann::json extra = nlohmann::json::object();  // 平台附加信息(用户名、隐私模式、webhook…)
};

struct Capabilities {
    std::size_t max_text_units = 4000;   // 单条文本上限
    bool count_utf16 = false;            // true:按 UTF-16 单位计数(Telegram)
    bool batch_turn_output = false;      // true:同一回合的助手文本先合并再发(QQ)
    bool supports_typing = false;
    std::uint64_t max_upload_bytes = 0;
    std::uint64_t max_download_bytes = 0;
};

enum class SendOutcome {
    Sent,     // 平台确认送达
    Held,     // 暂存,等对方下一条消息时补发(QQ 主动消息被拒)
    Failed,   // 平台明确拒绝或参数无效;不会重发
};

struct SendResult {
    SendOutcome outcome = SendOutcome::Failed;
    std::string error;
    bool ok() const { return outcome != SendOutcome::Failed; }
};

struct TransportCallbacks {
    // 在传输层自己的线程上调用;实现必须尽快返回(核心只入队)。
    std::function<void(Inbound)> on_inbound;
    std::function<void(const TransportStatus&)> on_status;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual std::string platform() const = 0;
    virtual Capabilities capabilities() const = 0;
    // 非阻塞:启动内部线程后立即返回。重复调用无副作用。
    virtual void start(TransportCallbacks callbacks) = 0;
    // 阻塞直到内部线程全部退出;之后不再回调。
    virtual void stop() = 0;
    virtual TransportStatus status() const = 0;
    // text 为 Markdown;由实现按平台格式化。reply_context 来自触发本回合的入站消息。
    virtual SendResult send_text(const Address& to, const std::string& text,
                                 const nlohmann::json& reply_context) = 0;
    virtual SendResult send_file(const Address& to, const std::filesystem::path& path,
                                 const std::string& name, const std::string& mime_type,
                                 const nlohmann::json& reply_context) = 0;
    virtual void set_typing(const Address& to, bool on) { (void)to; (void)on; }
    // 把入站附件下载到 dest(完整文件路径)。超出平台或本地上限时返回 false。
    virtual bool download(const Attachment& attachment, const std::filesystem::path& dest,
                          std::string* error) = 0;
    // 平台专属操作(如 Telegram 的 remove_webhook)。未知操作抛 std::runtime_error。
    virtual nlohmann::json action(const std::string& name, const nlohmann::json& args) {
        (void)args;
        throw std::runtime_error("Unsupported channel action: " + name);
    }
};

} // namespace acecode::im
