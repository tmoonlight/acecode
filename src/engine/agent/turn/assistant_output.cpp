#include "assistant_output.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/side_question/side_question_service.hpp"
#include "agent/hook_bridge/agent_hook_bridge.hpp"
#include "agent/model_step/model_step_recorder.hpp"
#include "llm/text_preamble_tags.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "session/session_manager.hpp"
#include "session/session_serializer.hpp"
#include "session/system_notice.hpp"
#include "utils/logger.hpp"

namespace acecode::agent {

void AssistantOutput::interrupted(const ChatResponse& output, SessionManager* session) {
    if (!output.content.empty() ||
        (output.content_parts.is_array() && !output.content_parts.empty())) {
        // Keep already displayed output across history reloads. An
        // interrupted response (especially partial tool calls) is not
        // a completed provider message, so it remains transcript-only.
        ChatMessage partial;
        partial.role = "assistant";
        partial.content = output.content;
        partial.content_parts = output.content_parts;
        partial.reasoning_content = output.reasoning_content;
        partial.metadata = {{"transcript_only", true}, {"interrupted_output", true}};
        if (session) session->on_message(partial);
        transcript_.dispatch_message(partial.role, partial.content, false,
                         partial.metadata, partial.content_parts);
    }

}

void AssistantOutput::completed(
    const ChatResponse& response, const ApiRequestBundle& bundle,
    const std::shared_ptr<LlmProvider>& provider_snapshot, int current_model_step,
    const TokenUsage& step_usage, SessionManager* session, HookManager* manager) {
    const bool truncated_by_length = response.finish_reason == "length";
    LOG_INFO("Text-only response; ending loop. content: " + log_truncate(response.content, 300));
    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.content = response.content;
    if (response.content_parts.is_array() && !response.content_parts.empty()) {
        assistant_msg.content_parts = response.content_parts;
    }
    assistant_msg.reasoning_content = response.reasoning_content;
    history_.append(assistant_msg);
    if (session) session->on_message(assistant_msg);
    auto completed_context = bundle.messages_with_system;
    completed_context.push_back(assistant_msg);
    side_questions_.publish(completed_context);
    {
        // 工具前言:模型若违规给最终回答也打了 <text_preamble> 标签,界面
        // 照样剥掉;落盘正文保留原文。
        const std::string visible_content =
            llm::strip_text_preamble_tags(
                response.content);
        const bool has_parts =
            response.content_parts.is_array() &&
            !response.content_parts.empty();
        if (!visible_content.empty() || has_parts) {
            transcript_.dispatch_message("assistant", visible_content, false,
                             nlohmann::json::object(),
                             response.content_parts);
        }
    }
    model_steps_.finish(
        current_model_step, response.finish_reason,
        step_usage);
    if (truncated_by_length) {
        transcript_.emit_transcript_system_message(session, 
            u8"[输出截断] 本回复因输出 token 上限被截断,内容可能不完整。",
            make_system_notice_metadata("response_truncated"));
    }
    hooks_.assistant_completed(manager, session, assistant_msg, provider_snapshot);

}

void AssistantOutput::ignored_text_call(
    const TextToolCallDiagnostic& diagnostic, SessionManager* session) {
    const std::string note = build_text_tool_call_ignored_note(
        diagnostic);
    if (!note.empty()) {
        ChatMessage ignored;
        ignored.role = "user";
        ignored.content = note;
        ignored.metadata = nlohmann::json{
            {"hidden_goal_context", true},
            {"text_tool_call_ignored", true},
        };
        ensure_user_message_identity(ignored);
        history_.append(ignored);
        if (session) session->on_message(ignored);
    }
}
} // namespace acecode::agent
