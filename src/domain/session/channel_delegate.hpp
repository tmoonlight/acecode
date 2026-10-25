#pragma once

// 群聊代答会话的身份(openspec add-channel-platform-plugins D5)。
// 消息通道把某个 IM 群绑定到一个会话时写进会话 meta 的 channel_delegate;
// AgentLoop 每次组装请求读取它:系统提示追加 <group-delegate> 块,工具收窄到只读内置工具
// 加 send_to_group_chat,MCP 工具全部不开放。字段为空 = 普通会话。

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_set>

namespace acecode {

inline constexpr const char* kSendToGroupChatToolName = "send_to_group_chat";

struct ChannelDelegate {
    std::string platform;   // 消息通道平台名
    std::string chat;       // 群 id
    std::string chat_name;  // 群名(可空)
    std::string user_name;  // 被代表的用户本人(可空)
    std::string prompt;     // 用户的代答约定(可空)

    bool active() const { return !platform.empty() && !chat.empty(); }
    bool operator==(const ChannelDelegate& other) const {
        return platform == other.platform && chat == other.chat && chat_name == other.chat_name &&
               user_name == other.user_name && prompt == other.prompt;
    }
    bool operator!=(const ChannelDelegate& other) const { return !(*this == other); }
};

nlohmann::json channel_delegate_to_json(const ChannelDelegate& delegate);
// 字段缺失或类型不对时返回不活动的身份(旧 meta 兼容)。
ChannelDelegate channel_delegate_from_json(const nlohmann::json& value);

// 追加在系统提示末尾的说明;只由 delegate 的内容决定,输入不变就逐字节不变。
std::string build_channel_delegate_system_context(const ChannelDelegate& delegate);

// 代答会话里保留的内置工具(与专家策略取交集)。
const std::unordered_set<std::string>& channel_delegate_builtin_tools();

} // namespace acecode
