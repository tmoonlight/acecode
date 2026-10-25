#pragma once

// send_to_group_chat(openspec add-channel-platform-plugins D5):群聊代答会话里把一段文字以用户
// 本人的身份发到绑定的群。只在 daemon 注册;非代答会话里由 AgentLoop 隐藏(schema 与执行同一谓词)。

#include "channels/core/conversations.hpp"
#include "tool/tool_executor.hpp"

#include <functional>
#include <string>

namespace acecode::channels::core {

using GroupSender = std::function<GroupSendResult(const std::string& session_id, const std::string& text)>;

ToolImpl create_send_to_group_chat_tool(GroupSender send);

} // namespace acecode::channels::core
