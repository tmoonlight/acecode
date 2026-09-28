#include "ask_question_binding.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "config/config.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/session_manager.hpp"
#include "utils/abort_signal.hpp"

namespace acecode::agent {

struct AskQuestionBinding::State {
    State(GoalRuntime& goal, AbortSignal& abort, const AgentLoopConfig& config,
        SessionManager* session, AskUserQuestionPrompter* prompter, AskQuestionChannel channel)
        : goal_(goal), abort_(abort), config_(config), session_manager_(session),
          prompter_(prompter), channel_(std::move(channel)) {}
    nlohmann::json ask_daemon(const nlohmann::json& payload, const std::string& tool,
        const std::string& id, int index, const ProgressEmitter& progress);
    GoalRuntime& goal_;
    AbortSignal& abort_;
    const AgentLoopConfig& config_;
    SessionManager* session_manager_; // Nullable borrowed; owner revokes before dependencies die.
    AskUserQuestionPrompter* prompter_; // Nullable borrowed.
    AskQuestionChannel channel_;
    LifetimeToken lifetime_;
};
AskQuestionBinding::AskQuestionBinding(GoalRuntime& goal, AbortSignal& abort,
    const AgentLoopConfig& config, SessionManager* session,
    AskUserQuestionPrompter* prompter, AskQuestionChannel channel)
    : state_(std::make_shared<State>(goal, abort, config, session, prompter, std::move(channel))) {}
AskQuestionBinding::~AskQuestionBinding() { state_->lifetime_.revoke(); }

void AskQuestionBinding::bind(ToolContext& context, const ToolCall& call,
    int index, const ProgressEmitter& progress) {
    const auto& state = *state_;
    const auto weak = std::weak_ptr<State>(state_);
    if (state.prompter_) {
        context.ask_user_questions =
            [weak, progress, tool = call.function_name, id = call.id, index](
                const nlohmann::json& payload) {
                nlohmann::json response{{"cancelled", true}};
                if (auto active = weak.lock()) {
                    active->lifetime_.ref(*active).with([&](State& binding) {
                        response = binding.ask_daemon(payload, tool, id, index, progress);
                    });
                }
                return response;
            };
    } else if (state.channel_) {
        // TUI binds timeout/origin now; daemon evaluates the goal when invoked.
        const auto policy = resolve_question_policy(state.config_);
        int timeout_seconds = 0;
        if (state.goal_.unattended_active(state.session_manager_)) {
            timeout_seconds = kGoalQuestionTimeoutSeconds;
        } else if (policy.policy == QuestionPolicy::Timeout) {
            timeout_seconds = policy.timeout_seconds;
        }
        std::string origin_label;
        if (state.session_manager_ && !state.session_manager_->current_parent_session_id().empty()) {
            const auto title = state.session_manager_->current_title();
            origin_label = "[subagent] " +
                (title.empty() ? state.session_manager_->current_session_id() : title);
        }
        context.ask_user_questions =
            [weak, timeout_seconds, origin_label](const nlohmann::json& payload) {
                nlohmann::json response{{"cancelled", true}};
                if (auto active = weak.lock()) {
                    active->lifetime_.ref(*active).with([&](State& binding) {
                        response = binding.channel_(payload, &binding.abort_.flag_for_legacy_api(),
                            timeout_seconds, origin_label);
                    });
                }
                return response;
            };
    }
}

nlohmann::json AskQuestionBinding::State::ask_daemon(
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
