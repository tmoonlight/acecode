#include "thread_repair.hpp"

#include "session_manager.hpp"
#include "../commands/compact.hpp"
#include "../utils/uuid.hpp"

#include <algorithm>
#include <cstddef>

namespace acecode {

namespace {

std::vector<std::size_t> real_user_starts(
    const std::vector<ChatMessage>& messages) {
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (is_real_user_message(messages[i])) starts.push_back(i);
    }
    return starts;
}

} // namespace

const char* to_string(ThreadRepairStatus status) {
    switch (status) {
        case ThreadRepairStatus::NoChange: return "noChange";
        case ThreadRepairStatus::Repaired: return "repaired";
        case ThreadRepairStatus::HistoryExhausted: return "historyExhausted";
        case ThreadRepairStatus::Failed: return "failed";
    }
    return "failed";
}

ThreadRepairResult plan_thread_repair(
    const std::vector<ChatMessage>& raw_messages,
    const ThreadRepairOptions& options,
    const SessionLoadDiagnostics& load_diagnostics) {
    ThreadRepairResult result;
    result.load_issues = load_diagnostics;

    auto recovered =
        reconstruct_effective_model_history_with_recovery(raw_messages);
    result.history_issues = recovered.stats;
    result.pre_tokens = estimate_message_tokens(recovered.messages);
    result.replacement_history = recovered.messages;

    const auto starts = real_user_starts(recovered.messages);
    std::size_t keep_from = 0;
    const bool target_requires_prune =
        options.target_tokens > 0 &&
        result.pre_tokens > options.target_tokens;
    while (result.pruned_groups + 1 < static_cast<int>(starts.size())) {
        const bool force_first = options.force_prune_one_group &&
                                 result.pruned_groups == 0;
        const int current_tokens = keep_from == 0
            ? result.pre_tokens
            : estimate_message_tokens(std::vector<ChatMessage>(
                  recovered.messages.begin() +
                      static_cast<std::ptrdiff_t>(keep_from),
                  recovered.messages.end()));
        const bool over_target = options.target_tokens > 0 &&
                                 current_tokens > options.target_tokens;
        if (!force_first && !over_target) break;
        ++result.pruned_groups;
        keep_from = starts[static_cast<std::size_t>(result.pruned_groups)];
    }

    if (keep_from > 0) {
        result.pruned_messages = static_cast<int>(keep_from);
        std::vector<ChatMessage> retained(
            recovered.messages.begin() +
                static_cast<std::ptrdiff_t>(keep_from),
            recovered.messages.end());
        result.replacement_history =
            recover_provider_history(retained).messages;
    }
    result.post_tokens = estimate_message_tokens(result.replacement_history);

    const bool recovery_changed = result.history_issues.changed() ||
                                  result.load_issues.recovered();
    if (result.pruned_groups == 0 && !recovery_changed) {
        const bool cannot_meet_target = target_requires_prune ||
            options.force_prune_one_group;
        result.status = cannot_meet_target
            ? ThreadRepairStatus::HistoryExhausted
            : ThreadRepairStatus::NoChange;
        result.reason = cannot_meet_target
            ? "no removable completed user-turn group remains"
            : "provider history is already consistent";
        return result;
    }

    result.checkpoint.id = generate_uuid();
    result.checkpoint.timestamp = iso_timestamp();
    result.checkpoint.trigger = options.trigger;
    result.checkpoint.summary = "Deterministic thread repair";
    result.checkpoint.messages_compressed = result.pruned_messages;
    result.checkpoint.estimated_tokens_saved =
        (std::max)(0, result.pre_tokens - result.post_tokens);
    result.checkpoint.pre_tokens = result.pre_tokens;
    result.checkpoint.post_tokens = result.post_tokens;
    result.checkpoint.replacement_history = result.replacement_history;
    result.status = ThreadRepairStatus::Repaired;
    result.reason = result.pruned_groups > 0
        ? "old completed user-turn groups were pruned"
        : "provider history structure was recovered";
    return result;
}

ThreadRepairResult apply_thread_repair(
    SessionManager* session_manager,
    std::vector<ChatMessage>& provider_history,
    const ThreadRepairOptions& options,
    const SessionLoadDiagnostics& load_diagnostics) {
    ThreadRepairResult result = plan_thread_repair(
        provider_history, options, load_diagnostics);
    if (!result.repaired()) return result;
    if (!session_manager) {
        result.status = ThreadRepairStatus::Failed;
        result.reason = "active session manager is unavailable";
        return result;
    }
    if (!session_manager->append_compact_checkpoint(result.checkpoint)) {
        result.status = ThreadRepairStatus::Failed;
        result.reason = "failed to append repair checkpoint";
        return result;
    }
    provider_history = result.replacement_history;
    return result;
}

nlohmann::json thread_repair_result_to_json(
    const ThreadRepairResult& result,
    const std::string& thread_id) {
    nlohmann::json issues{
        {"malformedToolCalls", result.history_issues.malformed_tool_calls},
        {"duplicateToolCalls", result.history_issues.duplicate_tool_calls},
        {"synthesizedToolResults", result.history_issues.synthesized_tool_results},
        {"standaloneToolResults", result.history_issues.standalone_tool_results},
        {"unexpectedToolResults", result.history_issues.unexpected_tool_results},
        {"duplicateToolResults", result.history_issues.duplicate_tool_results},
        {"emptyAssistantMessages", result.history_issues.empty_assistant_messages},
        {"malformedRecords", result.load_issues.malformed_complete_records},
        {"ignoredPartialTail", result.load_issues.ignored_partial_tail},
        {"recoveredUnterminatedRecord",
         result.load_issues.recovered_unterminated_record},
    };
    nlohmann::json out{
        {"status", to_string(result.status)},
        {"issues", std::move(issues)},
        {"preTokens", result.pre_tokens},
        {"postTokens", result.post_tokens},
        {"prunedGroups", result.pruned_groups},
        {"prunedMessages", result.pruned_messages},
        {"checkpointId", result.checkpoint.id.empty()
                             ? nlohmann::json(nullptr)
                             : nlohmann::json(result.checkpoint.id)},
        {"reason", result.reason},
    };
    if (!thread_id.empty()) out["threadId"] = thread_id;
    return out;
}

} // namespace acecode
