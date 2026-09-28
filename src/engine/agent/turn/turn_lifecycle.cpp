#include "turn_lifecycle.hpp"
#include "active_turn_gate.hpp"
#include "agent/agent_callbacks.hpp"
#include "session/event_dispatcher.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_queries.hpp"
#include "hooks/hook_runtime.hpp"
#include "llm/tool_protocol_names.hpp"
#include "permissions/shell_write_guard.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_rewind.hpp"
#include "session/session_storage.hpp"
#include "session/thread_goal_store.hpp"
#include "session/thread_repair.hpp"
#include "session/token_tracker.hpp"
#include "session/turn_timing.hpp"
#include "skills/skill_activation.hpp"
#include "skills/skill_usage_store.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"
#include "utils/text.hpp"
#include "utils/time.hpp"
#include "utils/uuid.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode::agent {

using agent::detail::trailing_transcript_message;
using utils::now_epoch_ms;

UserTurnInfo TurnLifecycle::prepare_user_turn(const UserInput& input,
                                                      bool hidden_goal_context) {
    UserTurnInfo info;
    info.turn_started_at_ms = now_epoch_ms();

    const std::string& user_message = input.text;
    const std::string& display_text = input.display_text;
    LOG_INFO("=== submit() user_message: " + log_truncate(user_message, 200));

    ExplicitSkillPromptExpansion skill_expansion;
    skill_expansion.prompt = user_message;
    if (!hidden_goal_context && skill_registry_ && !user_message.empty()) {
        skill_expansion = inject_explicit_skill_instructions(
            user_message, *skill_registry_);
        if (!skill_expansion.injected_skill_names.empty()) {
            LOG_INFO("[skills] Injected " +
                     std::to_string(skill_expansion.injected_skill_names.size()) +
                     " explicitly selected Skill prompt(s) for this turn");
            if (skill_usage_store_) {
                const std::string now = SessionStorage::now_iso8601();
                for (const auto& name :
                     skill_expansion.injected_skill_names) {
                    skill_usage_store_->record(name, now);
                }
            }
        }
    }
    const std::string& model_user_message = skill_expansion.prompt;

    // Add user message
    ChatMessage& user_msg = info.user_msg;
    user_msg.role = "user";
    user_msg.content = model_user_message;
    if (input.has_content_parts()) {
        user_msg.content_parts = input.content_parts;
        if (model_user_message != user_message) {
            bool replaced_text = false;
            for (auto& part : user_msg.content_parts) {
                if (!part.is_object() ||
                    part.value("type", std::string{}) != "text") {
                    continue;
                }
                if (part.value("text", std::string{}) == user_message) {
                    part["text"] = model_user_message;
                    replaced_text = true;
                    break;
                }
            }
            if (!replaced_text) {
                const std::string suffix =
                    model_user_message.rfind(user_message, 0) == 0
                        ? model_user_message.substr(user_message.size())
                        : model_user_message;
                if (!suffix.empty()) {
                    user_msg.content_parts.push_back(nlohmann::json{
                        {"type", "text"}, {"text", suffix}});
                }
            }
        }
    }
    if (input.metadata.is_object() && !input.metadata.empty()) {
        user_msg.metadata = input.metadata;
    }
    if (!display_text.empty() && display_text != user_message) {
        // 让 UI 渲染 display_text(原文),LLM 看到的仍是 user_message(展开后的)。
        // session_serializer 会把 metadata 全字段持久化,resume 后恢复。
        if (!user_msg.metadata.is_object()) user_msg.metadata = nlohmann::json::object();
        user_msg.metadata["display_text"] = display_text;
    } else if (model_user_message != user_message) {
        // Explicit Skill instructions are model context, not user-authored UI
        // text. Keep the original mention/request visible in the transcript.
        if (!user_msg.metadata.is_object()) user_msg.metadata = nlohmann::json::object();
        if (!user_msg.metadata.contains("display_text") ||
            !user_msg.metadata["display_text"].is_string()) {
            user_msg.metadata["display_text"] = user_message;
        }
    }
    if (hidden_goal_context) {
        if (!user_msg.metadata.is_object()) user_msg.metadata = nlohmann::json::object();
        user_msg.metadata["hidden_goal_context"] = true;
    }
    append_user_turn_message(info, hidden_goal_context);
    start_user_turn(info);
    return info;
}

void TurnLifecycle::append_user_turn_message(UserTurnInfo& info, bool hidden_goal_context) {
    transcript_.append_user_turn_message(session_manager_, info, hidden_goal_context);
}

UserTurnInfo TurnLifecycle::prepare_retry_user_turn(const ChatMessage& message) {
    UserTurnInfo info;
    info.user_msg = message;
    info.turn_started_at_ms = now_epoch_ms();
    const auto* tail = trailing_transcript_message(history_.view());
    if (tail && tail->role != "user") {
        // An aborted turn may already contain assistant/tool output. Preserve
        // it and append the original input with a fresh identity, without
        // expanding skills or attachments for a second time.
        info.user_msg.uuid.clear();
        info.user_msg.timestamp.clear();
        if (info.user_msg.metadata.is_object()) {
            for (const auto* key : {"client_message_id", "turn_steer", "turn_id",
                                    "turn_interrupt", "interrupted_turn_id"}) {
                info.user_msg.metadata.erase(key);
            }
        }
        append_user_turn_message(info, false);
        start_user_turn(info);
        return info;
    }
    info.active_turn_id = message.uuid;
    info.visible_timed_turn = true;
    info.turn_user_uuid = message.uuid;
    // Preserve the original checkpoint and message. Re-expansion or a second
    // on_message call would change the input or create adjacent user records.
    start_user_turn(info);
    return info;
}

void TurnLifecycle::start_user_turn(const UserTurnInfo& info) {
    if (session_manager_) {
        if (info.visible_timed_turn) {
            session_manager_->record_trajectory_event(
                "turn_start",
                {{"turn_id", info.active_turn_id},
                 {"user_message_id", info.turn_user_uuid},
                 {"started_at_ms", info.turn_started_at_ms}},
                info.turn_started_at_ms);
        }
        session_manager_->record_trajectory_event(
            "busy_changed",
            {{"busy", true}, {"turn_id", info.active_turn_id}});
    }
    gate_.begin(info.active_turn_id);
    if (callbacks_.on_busy_changed) {
        callbacks_.on_busy_changed(true);
    }
    events_.emit(SessionEventKind::BusyChanged, nlohmann::json{
        {"busy", true},
        {"turn_id", info.active_turn_id},
    });
}

} // namespace acecode::agent
