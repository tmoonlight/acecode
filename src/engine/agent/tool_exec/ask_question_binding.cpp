#include "ask_question_binding.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "config/config.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/session_manager.hpp"
#include "utils/abort_signal.hpp"

namespace acecode::agent {

void AskQuestionBinding::bind(ToolContext& context, const ToolCall& call,
    int index, const ProgressEmitter& progress) {
    const auto ref = lifetime_.ref(*this);
    if (prompter_) {
        context.ask_user_questions =
            [ref, progress, tool = call.function_name, id = call.id, index](
                const nlohmann::json& payload) {
                nlohmann::json response{{"cancelled", true}};
                ref.with([&](AskQuestionBinding& binding) {
                    response = binding.ask_daemon(payload, tool, id, index, progress);
                });
                return response;
            };
    } else if (channel_) {
        // TUI captures its timeout and origin at assembly; daemon checks the
        // unattended goal later, when the question is actually invoked.
        const auto policy = resolve_question_policy(config_);
        int timeout_seconds = 0;
        if (goal_.unattended_active(session_manager_)) {
            timeout_seconds = kGoalQuestionTimeoutSeconds;
        } else if (policy.policy == QuestionPolicy::Timeout) {
            timeout_seconds = policy.timeout_seconds;
        }
        std::string origin_label;
        if (session_manager_ && !session_manager_->current_parent_session_id().empty()) {
            const auto title = session_manager_->current_title();
            origin_label = "[subagent] " +
                (title.empty() ? session_manager_->current_session_id() : title);
        }
        context.ask_user_questions =
            [ref, channel = channel_, timeout_seconds, origin_label](
                const nlohmann::json& payload) {
                nlohmann::json response{{"cancelled", true}};
                ref.with([&](AskQuestionBinding& binding) {
                    response = channel(payload, &binding.abort_.flag_for_legacy_api(),
                        timeout_seconds, origin_label);
                });
                return response;
            };
    }
}

nlohmann::json AskQuestionBinding::ask_daemon(
    const nlohmann::json& questions_payload, const std::string& tool_name_for_question,
    const std::string& tool_call_id_for_question, int tool_index_int,
    const ProgressEmitter& emit_progress) {
    emit_progress("question_waiting", "正在等待用户回答",
        std::string{}, tool_name_for_question,
        tool_call_id_for_question, tool_index_int, true);
    std::optional<std::chrono::milliseconds> timeout_override;
    if (goal_.unattended_active(session_manager_)) {
        timeout_override = std::chrono::seconds(
            kGoalQuestionTimeoutSeconds);
    }
    AskUserQuestionResponse resp = prompter_->prompt(
        questions_payload, &abort_.flag_for_legacy_api(), timeout_override);
    nlohmann::json out;
    out["cancelled"] = resp.cancelled;
    // timeout 策略到期(add-ask-question-policy):工具侧据此
    // 合成「自动采纳每题第一选项」的结果。
    out["timed_out"] = resp.timed_out;
    // 用户在提问挂起时直接发文本(interject_question):
    // 工具侧据此给模型「改为直接输入,看下一条 user 消息」。
    out["interjected"] = resp.interjected;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& a : resp.answers) {
        nlohmann::json item;
        item["question_id"] = a.question_id;
        item["selected"]    = a.selected;
        item["custom_text"] = a.custom_text;
        // 对齐 TUI(ask_question_controller.cpp):selected 与
        // custom_text 均为空的题视为未作答,让 Web 端「跳过」
        // 在 LLM 结果中呈现为 "Not answered" 而非空串。
        item["not_answered"] = a.selected.empty() && a.custom_text.empty();
        arr.push_back(std::move(item));
    }
    out["answers"] = std::move(arr);
    return out;
}
} // namespace acecode::agent
