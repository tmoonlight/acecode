#include "thread_repair.hpp"

#include "session_manager.hpp"
#include "llm/message_predicates.hpp"
#include "llm/token_estimate.hpp"
#include "utils/uuid.hpp"

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

int single_message_tokens(const ChatMessage& message) {
    return estimate_message_tokens(std::vector<ChatMessage>{message});
}

// 精简老回合时删掉的消息:工具结果、带工具调用的助手消息、没有正文的助手
// 消息。用户消息(含压缩摘要、内部上下文)和助手的纯文本结论都留下 —— 只留
// 用户消息不留结论,模型会以为老任务还没做、从头再做一遍。
bool removed_when_thinning(const ChatMessage& message) {
    if (message.role == "tool") return true;
    if (message.role != "assistant") return false;
    if (message.tool_calls.is_array() && !message.tool_calls.empty()) {
        return true;
    }
    return message.content.find_first_not_of(" \t\r\n") == std::string::npos;
}

// 从最旧的老回合开始逐组精简,直到规模降到 target_tokens 以下(must_thin_one
// 时至少精简一组)。最后一组(含当前输入)不动。返回被精简的组数。
int thin_oldest_turns(std::vector<ChatMessage>& messages,
                      int target_tokens,
                      bool must_thin_one) {
    const auto starts = real_user_starts(messages);
    if (starts.size() < 2) return 0;
    std::vector<bool> drop(messages.size(), false);
    int current = estimate_message_tokens(messages);
    int thinned = 0;
    for (std::size_t group = 0; group + 1 < starts.size(); ++group) {
        const bool over_target = target_tokens > 0 && current > target_tokens;
        if (!over_target && !(must_thin_one && thinned == 0)) break;
        const std::size_t begin = group == 0 ? 0 : starts[group];
        bool changed = false;
        for (std::size_t i = begin; i < starts[group + 1]; ++i) {
            if (!removed_when_thinning(messages[i])) continue;
            drop[i] = true;
            current -= single_message_tokens(messages[i]);
            changed = true;
        }
        if (changed) ++thinned;
    }
    if (thinned == 0) return 0;
    std::vector<ChatMessage> kept;
    kept.reserve(messages.size());
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (!drop[i]) kept.push_back(std::move(messages[i]));
    }
    messages = std::move(kept);
    return thinned;
}

std::string repair_reason(const ThreadRepairResult& result) {
    std::vector<std::string> parts;
    if (result.cleared_tool_outputs > 0) {
        parts.push_back("old tool outputs were cleared");
    }
    if (result.thinned_groups > 0) {
        parts.push_back(
            "old turns were thinned to user messages and final replies");
    }
    if (result.pruned_groups > 0) {
        parts.push_back("old completed user-turn groups were pruned");
    }
    if (parts.empty()) return "provider history structure was recovered";
    std::string reason = parts.front();
    for (std::size_t i = 1; i < parts.size(); ++i) {
        reason += (i + 1 == parts.size() ? " and " : ", ") + parts[i];
    }
    return reason;
}

// 工具调用参数超过这个体积才值得清:小参数换成占位 JSON 省不了几个 token,
// 却让模型看不见自己刚才调了什么。
constexpr std::size_t kClearableToolArgumentsBytes = 4 * 1024;

bool tool_call_arguments_clearable(const ChatMessage& message) {
    if (message.role != "assistant" || !message.tool_calls.is_array()) {
        return false;
    }
    std::size_t bytes = 0;
    for (const auto& call : message.tool_calls) {
        if (!call.is_object()) continue;
        const auto function = call.find("function");
        if (function == call.end() || !function->is_object()) continue;
        const auto arguments = function->find("arguments");
        if (arguments == function->end()) continue;
        if (arguments->is_string()) {
            const std::string& text = arguments->get_ref<const std::string&>();
            if (text == kClearedToolArgumentsJson) continue;
            bytes += text.size();
        } else {
            bytes += arguments->dump().size();
        }
    }
    return bytes > kClearableToolArgumentsBytes;
}

void clear_tool_call_arguments(ChatMessage& message) {
    for (auto& call : message.tool_calls) {
        if (!call.is_object()) continue;
        auto function = call.find("function");
        if (function == call.end() || !function->is_object()) continue;
        if (function->contains("arguments")) {
            (*function)["arguments"] = kClearedToolArgumentsJson;
        }
    }
}

// 最近 keep_recent 条满足 matches 的消息之前的位置;这之前的才允许清。
// 按位置数而不是只数「值得清的」:最近一条工具结果哪怕很短也占一个名额,
// 否则保护会落到更早的一条大输出上,该清的反而清不掉。
template <typename Predicate>
std::size_t protected_tail_begin(const std::vector<ChatMessage>& messages,
                                 int keep_recent, Predicate matches) {
    std::size_t remaining =
        keep_recent < 0 ? 0 : static_cast<std::size_t>(keep_recent);
    std::size_t begin = messages.size();
    while (remaining > 0 && begin > 0) {
        --begin;
        if (matches(messages[begin])) --remaining;
    }
    return remaining > 0 ? 0 : begin;
}

// 从最旧往最新把工具输出换成占位符,直到历史规模降到 target_tokens 以下。
// 最近 keep_recent 条工具结果 / 工具调用不动。第一遍只清工具输出
// (role == "tool");不够再清体积大的工具调用参数。返回被清的条数。
int clear_oldest_tool_outputs(std::vector<ChatMessage>& messages,
                              int target_tokens,
                              int keep_recent) {
    int current = estimate_message_tokens(messages);
    if (current <= target_tokens) return 0;
    int cleared = 0;

    // 比占位符还短的输出不值得清:换上去反而更长。
    const std::size_t placeholder_bytes =
        std::char_traits<char>::length(kClearedToolOutputPlaceholder);
    const std::size_t outputs_end = protected_tail_begin(
        messages, keep_recent,
        [](const ChatMessage& message) { return message.role == "tool"; });
    for (std::size_t i = 0; i < outputs_end && current > target_tokens; ++i) {
        ChatMessage& message = messages[i];
        if (message.role != "tool" ||
            message.content == kClearedToolOutputPlaceholder ||
            message.content.size() <= placeholder_bytes) {
            continue;
        }
        const int before = single_message_tokens(message);
        message.content = kClearedToolOutputPlaceholder;
        message.content_parts = nlohmann::json();
        current -= before - single_message_tokens(message);
        ++cleared;
    }
    if (current <= target_tokens) return cleared;

    const std::size_t calls_end = protected_tail_begin(
        messages, keep_recent, [](const ChatMessage& message) {
            return message.role == "assistant" &&
                   message.tool_calls.is_array() &&
                   !message.tool_calls.empty();
        });
    for (std::size_t i = 0; i < calls_end && current > target_tokens; ++i) {
        ChatMessage& message = messages[i];
        if (!tool_call_arguments_clearable(message)) continue;
        const int before = single_message_tokens(message);
        clear_tool_call_arguments(message);
        current -= before - single_message_tokens(message);
        ++cleared;
    }
    return cleared;
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

    const bool target_requires_prune =
        options.target_tokens > 0 &&
        result.pre_tokens > options.target_tokens;
    std::vector<ChatMessage> working = recovered.messages;

    // 第一步:清工具输出(与调用参数)。工具输出最占地方又能重新获取,先清它,
    // 老回合里的任务说明与纠正才留得住。
    if (options.clear_tool_outputs && options.target_tokens > 0) {
        result.cleared_tool_outputs = clear_oldest_tool_outputs(
            working, options.target_tokens, options.keep_recent_tool_outputs);
    }
    // force_prune_one_group 保证「修复即有进展」:前面已经腾出空间就不再强制丢组。
    bool must_make_progress =
        options.force_prune_one_group && result.cleared_tool_outputs == 0;

    // 第二步:精简老回合,只删工具往返,保留用户消息、摘要与纯文本结论。
    if (options.thin_old_turns_first) {
        result.thinned_groups = thin_oldest_turns(
            working, options.target_tokens, must_make_progress);
        if (result.thinned_groups > 0) must_make_progress = false;
    }

    // 第三步:整组丢弃最旧的回合。精简模式下保留被丢部分里最新的一份压缩
    // 摘要 —— 它是之前所有回合的唯一记忆。
    const auto starts = real_user_starts(working);
    std::size_t keep_from = 0;
    auto retained_view = [&](std::size_t from) {
        std::vector<ChatMessage> retained;
        const bool retained_has_summary = std::any_of(
            working.begin() + static_cast<std::ptrdiff_t>(from), working.end(),
            [](const ChatMessage& message) {
                return is_compact_summary_message(message);
            });
        if (options.thin_old_turns_first && !retained_has_summary) {
            for (std::size_t i = from; i > 0; --i) {
                if (is_compact_summary_message(working[i - 1])) {
                    retained.push_back(working[i - 1]);
                    break;
                }
            }
        }
        retained.insert(retained.end(),
                        working.begin() + static_cast<std::ptrdiff_t>(from),
                        working.end());
        return retained;
    };
    int current_tokens = estimate_message_tokens(working);
    while (result.pruned_groups + 1 < static_cast<int>(starts.size())) {
        const bool force_first = must_make_progress && result.pruned_groups == 0;
        const bool over_target = options.target_tokens > 0 &&
                                 current_tokens > options.target_tokens;
        if (!force_first && !over_target) break;
        ++result.pruned_groups;
        keep_from = starts[static_cast<std::size_t>(result.pruned_groups)];
        current_tokens = estimate_message_tokens(retained_view(keep_from));
    }

    if (keep_from > 0 || result.cleared_tool_outputs > 0 ||
        result.thinned_groups > 0) {
        const std::vector<ChatMessage> retained = retained_view(keep_from);
        result.pruned_messages = static_cast<int>(
            recovered.messages.size() - (std::min)(recovered.messages.size(),
                                                   retained.size()));
        result.replacement_history =
            recover_provider_history(retained).messages;
    }
    result.post_tokens = estimate_message_tokens(result.replacement_history);

    const bool recovery_changed = result.history_issues.changed() ||
                                  result.load_issues.recovered();
    if (result.pruned_groups == 0 && result.cleared_tool_outputs == 0 &&
        result.thinned_groups == 0 && !recovery_changed) {
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
    result.reason = repair_reason(result);
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
        {"clearedToolOutputs", result.cleared_tool_outputs},
        {"checkpointId", result.checkpoint.id.empty()
                             ? nlohmann::json(nullptr)
                             : nlohmann::json(result.checkpoint.id)},
        {"reason", result.reason},
    };
    if (!thread_id.empty()) out["threadId"] = thread_id;
    return out;
}

} // namespace acecode
