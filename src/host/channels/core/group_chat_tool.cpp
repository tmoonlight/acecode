#include "group_chat_tool.hpp"

#include "session/channel_delegate.hpp"

#include <nlohmann/json.hpp>

namespace acecode::channels::core {

ToolImpl create_send_to_group_chat_tool(GroupSender send) {
    ToolImpl tool;
    tool.definition.name = kSendToGroupChatToolName;
    tool.definition.description =
        "Send one message to the IM group chat bound to this session, as the user you are representing. "
        "This is the ONLY way anything reaches the group; your normal replies stay in this session. "
        "Call it only when the latest group messages concern that user and need a reply under the delegate "
        "rules. Keep it short and natural, in the user's own voice. At most 2000 characters per message and "
        "5 messages per digest.";
    tool.definition.parameters = {
        {"type", "object"},
        {"properties", {{"text", {{"type", "string"}, {"description", "Message text to send to the group."}}}}},
        {"required", nlohmann::json::array({"text"})},
        {"additionalProperties", false}};
    // 用户在设置页开启群聊代答即授权代发;工具只能发到本会话绑定的群,长度与次数都有上限,
    // 所以不再逐次弹权限确认(代答会话无人值守,弹了也没人点)。
    tool.is_read_only = true;
    tool.requires_serial_execution = true;
    tool.execute = [send](const std::string& arguments, const ToolContext& ctx) {
        const auto body = nlohmann::json::parse(arguments, nullptr, false);
        if (!body.is_object() || !body.contains("text") || !body["text"].is_string())
            return ToolResult{"[Error] send_to_group_chat needs {\"text\": \"...\"}", false};
        if (ctx.session_id.empty()) return ToolResult{"[Error] No session is bound to a group chat.", false};
        if (!send) return ToolResult{"[Error] Message channels are unavailable.", false};
        const auto result = send(ctx.session_id, body["text"].get<std::string>());
        if (!result.ok) return ToolResult{"[Error] " + result.error, false};
        return ToolResult{"已发送到群聊。", true};
    };
    return tool;
}

} // namespace acecode::channels::core
