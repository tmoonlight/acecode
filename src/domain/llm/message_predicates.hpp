#pragma once

// 会话历史里「哪条是真正的用户消息 / 哪条是压缩摘要」的谓词(P2-02 自 commands/compact.hpp 拆出),
// 以及摘要消息的固定前缀。domain 层不含压缩流程本身。

#include "llm/llm_provider.hpp"

#include <string>

namespace acecode {

// Codex 检查点契约里摘要消息开头的固定文案,用来识别历史中的压缩摘要。
const std::string& get_compact_summary_prefix();

bool is_real_user_message(const ChatMessage& msg);
bool is_compact_summary_message(const ChatMessage& msg);

// 后台任务(后台命令 / 后台子代理)的通知:落盘为 user 角色 + metadata.background_task,
// 和网状蜂群的非 NEW_TASK 信封一样属内部上下文,不是用户发言(不计回合、不当标题 /
// 摘要、不触发 UserPromptSubmit、不建检查点)。
inline constexpr const char* kBackgroundTaskMetadataKey = "background_task";
bool is_background_task_metadata(const nlohmann::json& metadata);
bool is_background_task_message(const ChatMessage& msg);
// TUI / 消息通道等纯文本界面显示的说明(标题行 + 通知正文);不是后台任务通知时为空。
std::string background_task_display_text(const ChatMessage& msg);

} // namespace acecode
