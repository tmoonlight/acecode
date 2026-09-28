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

} // namespace acecode
