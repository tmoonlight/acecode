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

struct ThreadRepairOptions {
    std::string trigger = "repair-manual";
    int target_tokens = 0;
    bool force_prune_one_group = false;
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
