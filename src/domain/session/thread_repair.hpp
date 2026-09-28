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
    // 老回合丢完(或只剩当前回合)仍超 target_tokens 时,继续在保留的历史里
    // 把最旧的工具输出换成占位符腾空间;工具输出清完还不够,再把体积超过
    // 4KB 的工具调用参数(典型是 file_write 的整篇文件内容)换成占位 JSON。
    // 两遍都从最旧往最新清,最近 keep_recent_tool_outputs 条工具结果 / 调用
    // 不动 —— 模型要靠最新那条结果继续干活。默认关闭,只有明确要「不惜代价
    // 腾出空间」的调用方(PA 兜底、摘要压缩失败后的机械兜底)打开。
    bool clear_tool_outputs = false;
    int keep_recent_tool_outputs = 1;
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
