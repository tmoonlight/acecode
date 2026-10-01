#include "tool_result_committer.hpp"
#include "tool_lifecycle_events.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "session/session_manager.hpp"
#include "session/tool_metadata_codec.hpp"
#include "tool/mtime_tracker.hpp"
#include "tool/tool_executor.hpp"
#include "utils/logger.hpp"

namespace acecode::agent {
ToolBatchOutcome ToolResultCommitter::commit(std::vector<ToolCallSlot>& slots) {
    std::vector<ToolResultReplacementRecord> replacement_records;
    for (auto& slot : slots) {
        if (slot.outcome && !slot.outcome->delivery_replacement.tool_call_id.empty()) {
            replacement_records.push_back(
                std::move(slot.outcome->delivery_replacement));
        }
    }
    if (session_manager_) {
        const std::string tool_results_dir = session_manager_->ensure_tool_results_dir();
        if (!tool_results_dir.empty()) {
            auto replacement_state = reconstruct_tool_result_replacement_state(history_.view());
            std::vector<ToolResultBudgetEntry> entries;
            for (auto& slot : slots) {
                if (slot.outcome) {
                    entries.push_back({slot.call, slot.outcome->result});
                }
            }
            auto budget_result = enforce_tool_result_budget(
                entries, tool_results_dir, replacement_state);
            for (auto& record : budget_result.newly_replaced) {
                replacement_records.push_back(std::move(record));
            }
        }
    }

    // 与 file_read 记录观测时同一口径(ToolContextFactory 填的会话 id)。
    const std::string read_scope =
        session_manager_ ? session_manager_->current_session_id() : std::string{};
    auto record_file_read_result_reference = [&read_scope](MtimeTracker& tracker, const ToolCall& tc, const ToolResult& result) {
        if (!result.success || tc.function_name != "file_read") return;
        if (result.output.rfind("File unchanged since last read.", 0) == 0) return;

        auto args = nlohmann::json::parse(tc.function_arguments, nullptr, false);
        if (!args.is_object() ||
            !args.contains("file_path") ||
            !args["file_path"].is_string()) {
            return;
        }

        auto int_arg = [&args](const char* key) -> int {
            if (!args.contains(key) || !args[key].is_number_integer()) return 0;
            return args[key].get<int>();
        };
        auto uint64_arg = [&args](const char* key) -> uint64_t {
            if (!args.contains(key)) return 0;
            if (args[key].is_number_unsigned()) return args[key].get<uint64_t>();
            if (!args[key].is_number_integer()) return 0;
            const auto value = args[key].get<int64_t>();
            return value >= 0 ? static_cast<uint64_t>(value) : 0;
        };
        const bool byte_mode = args.contains("byte_offset");

        tracker.record_read_observation_result(
            args["file_path"].get<std::string>(),
            int_arg("start_line"),
            int_arg("end_line"),
            tc.id,
            persisted_output_filepath(result.output),
            byte_mode,
            uint64_arg("byte_offset"),
            static_cast<size_t>(uint64_arg("max_bytes")),
            read_scope);
    };

    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].outcome) {
            record_file_read_result_reference(environment_.mtime_tracker(), slots[i].call, slots[i].outcome->result);
        }
    }

    // Phase 3: Record and dispatch all results in original order
    for (size_t i = 0; i < slots.size(); ++i) {
        const auto& tc = slots[i].call;
        ChatMessage tool_msg;
        if (slots[i].outcome) {
            tool_msg = ToolExecutor::format_tool_result(tc.id, slots[i].outcome->result);
            if (slots[i].outcome->result.summary.has_value()) {
                tool_msg.metadata["tool_summary"] = encode_tool_summary(*slots[i].outcome->result.summary);
            }
            if (slots[i].outcome->result.hunks.has_value()) {
                tool_msg.metadata["tool_hunks"] = encode_tool_hunks(*slots[i].outcome->result.hunks);
            }
        } else {
            ToolResult interrupted_result{"[Interrupted]", false};
            ensure_tool_summary(
                tc.function_name, tc.function_arguments, interrupted_result);
            tool_msg = ToolExecutor::format_tool_result(
                tc.id, interrupted_result);
            // AskUserQuestion deliberately has no synthesized summary, so the
            // metadata key must stay absent instead of dereferencing nullopt.
            if (interrupted_result.summary.has_value()) {
                tool_msg.metadata["tool_summary"] =
                    encode_tool_summary(*interrupted_result.summary);
            }
        }
        history_.append(tool_msg);
        if (session_manager_) session_manager_->on_message(tool_msg);

        if (tc.function_name == "task_complete" &&
            slots[i].outcome && slots[i].outcome->result.success) {
            lifecycle_events_.finish_deferred(
                tc, static_cast<int>(i), *slots[i].outcome, tool_msg);
        }

        // 展示派发(tool_result 伪行 + on_tool_result)已前移到各执行点
        // (dispatch_tool_result_display),这里只保留 canonical 相关处理。
        if (slots[i].outcome) {
            if (slots[i].outcome->result.post_user_prompt.has_value() &&
                !slots[i].outcome->result.post_user_prompt->empty()) {
                transcript_.append_tool_user_prompt(session_manager_,
                    *slots[i].outcome->result.post_user_prompt,
                    slots[i].outcome->result.post_user_prompt_display_text,
                    tc.function_name);
            }
        }
    }

    if (!replacement_records.empty()) {
        ChatMessage meta_msg = encode_content_replacement_message(replacement_records);
        history_.append(meta_msg);
        if (session_manager_) session_manager_->on_message(meta_msg);
    }

    agent::ToolBatchOutcome batch_outcome;
    // A session-terminal action takes precedence over ordinary terminators.
    // Move its callback only after every canonical result has been recorded.
    for (size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i].outcome ||
            !slots[i].outcome->result.terminate_session_after_turn) {
            continue;
        }
        batch_outcome.terminate_session_after_turn = true;
        if (slots[i].outcome->result.post_turn_action) {
            batch_outcome.post_turn_actions.push_back(
                std::move(slots[i].outcome->result.post_turn_action));
        }
        LOG_INFO("Terminal session action queued after turn boundary");
    }
    if (batch_outcome.terminate_session_after_turn) {
        batch_outcome.terminator_fired = true;
        return batch_outcome;
    }

    // Terminator detection. A failed ExitPlanMode is a user/runtime boundary:
    // retrying it in the same turn only replays the approval request while the
    // session correctly remains in Plan mode.
    for (size_t i = 0; i < slots.size(); ++i) {
        const auto& tc = slots[i].call;
        if (tc.function_name == "task_complete" && slots[i].outcome && slots[i].outcome->result.success) {
            LOG_INFO("Terminator fired: task_complete");
            batch_outcome.terminator_fired = true;
            return batch_outcome;
        }
        if (tc.function_name == "ExitPlanMode" && slots[i].outcome && !slots[i].outcome->result.success) {
            LOG_INFO("Ending turn after failed ExitPlanMode");
            batch_outcome.terminator_fired = true;
            return batch_outcome;
        }
    }
    return batch_outcome;
}
} // namespace acecode::agent
