#pragma once

#include "compact_checkpoint.hpp"
#include "session_storage.hpp"

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode {

class SessionManager;

enum class ThreadRepairStatus {
    NoChange,
    Repaired,
    HistoryExhausted,
    Failed,
};

// 被清除的工具输出 / 工具调用参数在模型历史里的占位文本。模型可见,所以要
// 告诉它结果没了、需要的话重新执行。
inline constexpr const char* kClearedToolOutputPlaceholder =
    "[tool output cleared to fit the provider context limit; re-run the tool "
    "if you still need its result]";
inline constexpr const char* kClearedToolArgumentsJson =
    R"({"_cleared":"arguments removed to fit the provider context limit"})";

struct ThreadRepairOptions {
    std::string trigger = "repair-manual";
    int target_tokens = 0;
    bool force_prune_one_group = false;
    // 超 target_tokens 时**先于**丢老回合,把整段历史里最旧的工具输出换成
    // 占位符腾空间;工具输出清完还不够,再把体积超过 4KB 的工具调用参数
    // (典型是 file_write 的整篇文件内容)换成占位 JSON。两遍都从最旧往最新清,
    // 最近 keep_recent_tool_outputs 条工具结果 / 调用不动 —— 模型要靠最新那条
    // 结果继续干活。默认关闭,只有明确要「不惜代价腾出空间」的调用方(PA 兜底、
    // 摘要压缩失败后的机械兜底)打开。
    // 先清输出、后丢回合:工具输出是历史里最占地方、也最能重新获取的部分;
    // 老回合里的用户消息才是任务要求与纠正,丢了模型就「失忆」(反馈
    // LINDANDAN069:先丢整组回合的旧顺序把最初的任务说明、用户的多次纠正和
    // 压缩摘要一起删掉,而实际省下的空间主要来自随后清掉的工具输出)。
    bool clear_tool_outputs = false;
    int keep_recent_tool_outputs = 1;
    // 需要丢老回合时先「精简」:只删其中的工具调用与工具结果,保留用户消息、
    // 压缩摘要和助手的纯文本结论;精简完仍超目标才整组丢弃,而且整组丢弃时
    // 保留最新一份压缩摘要。默认关闭(手动修复与通用恢复链保持整组丢弃)。
    bool thin_old_turns_first = false;
};

struct ThreadRepairResult {
    ThreadRepairStatus status = ThreadRepairStatus::NoChange;
    ProviderHistoryRecoveryStats history_issues;
    SessionLoadDiagnostics load_issues;
    std::vector<ChatMessage> replacement_history;
    CompactCheckpoint checkpoint;
    int pre_tokens = 0;
    int post_tokens = 0;
    int pruned_groups = 0;
    int pruned_messages = 0;
    // clear_tool_outputs 打开时被换成占位符的工具输出 + 工具调用参数条数。
    int cleared_tool_outputs = 0;
    // thin_old_turns_first 打开时被精简(删掉工具往返、保留用户消息与结论)的老回合数。
    int thinned_groups = 0;
    std::string reason;

    bool repaired() const { return status == ThreadRepairStatus::Repaired; }
};

ThreadRepairResult plan_thread_repair(
    const std::vector<ChatMessage>& raw_messages,
    const ThreadRepairOptions& options,
    const SessionLoadDiagnostics& load_diagnostics = {});

// Worker-boundary helper for active sessions. It appends a checkpoint through
// SessionManager and swaps only the in-memory provider projection.
ThreadRepairResult apply_thread_repair(
    SessionManager* session_manager,
    std::vector<ChatMessage>& provider_history,
    const ThreadRepairOptions& options,
    const SessionLoadDiagnostics& load_diagnostics = {});

nlohmann::json thread_repair_result_to_json(
    const ThreadRepairResult& result,
    const std::string& thread_id = {});

const char* to_string(ThreadRepairStatus status);

} // namespace acecode
