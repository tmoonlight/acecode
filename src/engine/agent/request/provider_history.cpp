#include "provider_history.hpp"
#include "session/compact_checkpoint.hpp"
#include "session/session_history_recovery.hpp"
#include "llm/tool_protocol_names.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "agent/compaction/compact_prompt.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <utility>

namespace acecode::agent::detail {

std::vector<ChatMessage> recovered_provider_messages(
    const std::vector<ChatMessage>& messages,
    const char* boundary) {
    auto recovery = recover_provider_history(provider_relevant_messages(messages));
    if (recovery.stats.changed()) {
        const auto& stats = recovery.stats;
        LOG_WARN(std::string{"[session-recovery] boundary="} + boundary +
                 " malformed_calls=" + std::to_string(stats.malformed_tool_calls) +
                 " duplicate_calls=" + std::to_string(stats.duplicate_tool_calls) +
                 " synthesized_results=" +
                 std::to_string(stats.synthesized_tool_results) +
                 " standalone_results=" +
                 std::to_string(stats.standalone_tool_results) +
                 " unexpected_results=" +
                 std::to_string(stats.unexpected_tool_results) +
                 " duplicate_results=" +
                 std::to_string(stats.duplicate_tool_results) +
                 " empty_assistants=" +
                 std::to_string(stats.empty_assistant_messages));
    }
    return std::move(recovery.messages);
}

// 发给模型的历史**唯一入口**:先做历史修复,再把 tool_calls 的名字改写成
// 模型侧名(「工具重写」生效时才有差异)。新增任何「构造 provider 消息」
// 的路径都必须走这里 —— 曾经 side-question 与主请求各自拼装,漏掉改写的
// 那条路径会让模型看到它工具表里没有的原生名。
std::vector<ChatMessage> model_facing_provider_messages(
    const std::vector<ChatMessage>& messages,
    const char* boundary) {
    auto history = recovered_provider_messages(messages, boundary);
    // 旧的纯文本工具调用 / 被污染的摘要换成固定说明(只由内容决定、逐字节
    // 稳定),模型不再照着历史里的样本继续写文本调用。
    sanitize_text_tool_call_history(history, get_compact_summary_prefix());
    rewrite_tool_calls_for_model(history);
    return history;
}

} // namespace acecode::agent::detail
