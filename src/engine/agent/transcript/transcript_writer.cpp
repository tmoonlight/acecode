#include "transcript_writer.hpp"
#include "agent/callbacks_slot.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/turn/turn_outcome.hpp"
#include "agent/event_payload/message_payload.hpp"
#include "session/event_dispatcher.hpp"
#include "session/session_manager.hpp"
#include "session/session_rewind.hpp"
#include "session/session_storage.hpp"
#include "session/turn_timing.hpp"
#include "utils/logger.hpp"
#include <algorithm>
#include <sstream>
#include <utility>

namespace acecode::agent {

void TranscriptWriter::dispatch_message(const std::string& role,
                                  const std::string& content,
                                  bool is_tool,
                                  nlohmann::json metadata,
                                  nlohmann::json content_parts) {
    const auto callbacks = callbacks_.snapshot();
    if (role == "error") {
        // 回合级错误文案的唯一收集点:provider 终止错误 / 压缩失败 / 空回复
        // 耗尽 / hook 拦截都经这里派发,wait_subagent 报 ChildFailed 时带上。
        outcome_.set_error(content);
    }
    if (callbacks.on_message) {
        callbacks.on_message(role, content, is_tool);
    }
    // Web 协议给每条 message 带稳定 id:user 走持久 uuid(走另一路径
    // 直接 emit,见 run_agent),其它角色 lazy sha1(role + " " + content
    // + " " + timestamp)。这里 timestamp 默认空字符串,跟磁盘上 JSONL
    // 重读时算出来的 ID 保持一致(JSONL 里 assistant 消息也没 timestamp)。
    ChatMessage tmp;
    tmp.role    = role;
    tmp.content = content;
    if (content_parts.is_array() && !content_parts.empty()) {
        tmp.content_parts = content_parts;
    }
    nlohmann::json payload = {
        {"role", role}, {"content", content}, {"is_tool", is_tool},
        {"id", web::compute_message_id(tmp)}};
    if (content_parts.is_array() && !content_parts.empty()) {
        payload["content_parts"] = std::move(content_parts);
    }
    if (metadata.is_object() && !metadata.empty()) {
        payload["metadata"] = std::move(metadata);
    }
    events_.emit(SessionEventKind::Message, std::move(payload));
}

void TranscriptWriter::append_turn_timing_record(SessionManager* session, const std::string& user_message_uuid,
                                          std::int64_t started_at_ms,
                                          std::int64_t completed_at_ms,
                                          const std::string& status) {
    if (user_message_uuid.empty()) return;
    TurnTimingRecord timing;
    timing.user_message_uuid = user_message_uuid;
    timing.started_at_ms = started_at_ms;
    timing.completed_at_ms = completed_at_ms;
    timing.duration_ms = std::max<std::int64_t>(0, completed_at_ms - started_at_ms);
    timing.status = status;

    ChatMessage msg = make_turn_timing_message(timing, SessionStorage::now_iso8601());
    history_.append(msg);
    if (session) {
        session->on_message(msg);
        session->record_trajectory_event(
            "turn_end",
            {{"turn_id", timing.user_message_uuid},
             {"user_message_id", timing.user_message_uuid},
             {"started_at_ms", timing.started_at_ms},
             {"completed_at_ms", timing.completed_at_ms},
             {"duration_ms", timing.duration_ms},
             {"outcome", timing.status}},
            timing.completed_at_ms);
    }
}

void TranscriptWriter::append_tool_user_prompt(SessionManager* session, const std::string& content,
                                        const std::string& display_text,
                                        const std::string& source_tool) {
    const auto callbacks = callbacks_.snapshot();
    if (content.empty()) return;

    ChatMessage msg;
    msg.role = "user";
    msg.content = content;
    msg.metadata = nlohmann::json::object();
    msg.metadata["display_text"] = display_text.empty()
        ? "[Tool prompt loaded]"
        : display_text;
    msg.metadata["synthetic_user_prompt"] = true;
    if (!source_tool.empty()) msg.metadata["source_tool"] = source_tool;
    ensure_user_message_identity(msg);

    history_.append(msg);
    if (session) {
        session->on_message(msg);
    }

    if (callbacks.on_message) {
        callbacks.on_message("user", msg.metadata.value("display_text", msg.content), false);
    }
    nlohmann::json event = {
        {"role", "user"},
        {"content", msg.content},
        {"is_tool", false},
        {"id", msg.uuid},
        {"metadata", msg.metadata},
    };
    events_.emit(SessionEventKind::Message, std::move(event));
}

void TranscriptWriter::emit_system_message(const std::string& content, nlohmann::json metadata) {
    dispatch_message("system", content, false, std::move(metadata), nlohmann::json::array());
}

void TranscriptWriter::emit_transcript_system_message(SessionManager* session, const std::string& content,
                                               nlohmann::json metadata) {
    const auto callbacks = callbacks_.snapshot();
    ChatMessage msg;
    msg.role = "system";
    msg.content = content;
    msg.timestamp = SessionStorage::now_iso8601();
    msg.metadata = metadata.is_object() ? std::move(metadata) : nlohmann::json::object();
    msg.metadata["transcript_only"] = true;

    if (callbacks.on_transcript_message) {
        callbacks.on_transcript_message(msg);
    } else if (callbacks.on_message) {
        callbacks.on_message(msg.role, msg.content, false);
    }
    if (session) {
        session->on_message(msg);
    }

    nlohmann::json payload = {
        {"role", msg.role},
        {"content", msg.content},
        {"is_tool", false},
        {"id", web::compute_message_id(msg)},
        {"timestamp", msg.timestamp},
        {"metadata", msg.metadata},
    };
    events_.emit(SessionEventKind::Message, std::move(payload));
}

void TranscriptWriter::inject_shell_turn(const std::string& cmd,
                                  const std::string& stdout_text,
                                  const std::string& stderr_text,
                                  int exit_code) {
    ChatMessage msg;
    msg.role = "user";
    std::ostringstream oss;
    oss << "<bash-input>" << cmd << "</bash-input>\n"
        << "<bash-stdout>" << stdout_text << "</bash-stdout>\n"
        << "<bash-stderr>" << stderr_text << "</bash-stderr>\n"
        << "<bash-exit-code>" << exit_code << "</bash-exit-code>";
    msg.content = oss.str();
    history_.append(std::move(msg));
}

void TranscriptWriter::emit_session_summary_updated(SessionManager* session) {
    if (!session) return;
    const std::string summary = session->current_summary();
    if (summary.empty()) return;
    events_.emit(SessionEventKind::SessionUpdated,
                 nlohmann::json{{"summary", summary}});
}

void TranscriptWriter::append_user_turn_message(SessionManager* session, UserTurnInfo& info, bool hidden_goal_context) {
    auto& user_msg = info.user_msg;
    ensure_user_message_identity(user_msg);
    info.active_turn_id = user_msg.uuid;
    info.visible_timed_turn =
        !hidden_goal_context &&
        !(user_msg.metadata.is_object() && user_msg.metadata.value("hidden_goal_context", false));
    info.turn_user_uuid = info.visible_timed_turn ? user_msg.uuid : std::string{};

    history_.append(user_msg);
    if (session) {
        session->on_message(user_msg);
        if (!hidden_goal_context) {
            session->begin_user_turn_checkpoint(user_msg.uuid);
        }
    }
    if (!hidden_goal_context) {
        emit_session_summary_updated(session);
        nlohmann::json msg_event = {
            {"role", "user"}, {"content", user_msg.content},
            {"is_tool", false}, {"id", user_msg.uuid},
        };
        if (!user_msg.content_parts.is_null() && user_msg.content_parts.is_array() &&
            !user_msg.content_parts.empty()) {
            msg_event["content_parts"] = user_msg.content_parts;
        }
        if (!user_msg.metadata.is_null() && !user_msg.metadata.empty()) {
            msg_event["metadata"] = user_msg.metadata;
        }
        events_.emit(SessionEventKind::Message, msg_event);
    }
}

void TranscriptWriter::append_interrupted_turn_context(SessionManager* session, const std::string& turn_id) {
    ChatMessage marker;
    marker.role = "user";
    marker.content =
        "<turn_aborted>\n"
        "The user interrupted the previous turn on purpose to submit new "
        "instructions. Any running tools or commands may have partially "
        "executed; inspect their state before retrying.\n"
        "</turn_aborted>";
    marker.metadata = nlohmann::json{
        {"hidden_goal_context", true},
        {"turn_interrupt_marker", true},
        {"interrupted_turn_id", turn_id},
    };
    ensure_user_message_identity(marker);
    history_.append(marker);
    if (session) session->on_message(marker);
    LOG_INFO("[turn/interrupt] recorded interrupted-turn context for " + turn_id);
}

void TranscriptWriter::commit_turn_steering_input(SessionManager* session,
    UserInput input,
    const std::string& turn_id) {
    const auto callbacks = callbacks_.snapshot();
    ChatMessage message;
    message.role = "user";
    message.content = std::move(input.text);
    message.content_parts = std::move(input.content_parts);
    message.metadata = std::move(input.metadata);
    if (!message.metadata.is_object()) {
        message.metadata = nlohmann::json::object();
    }
    if (!input.display_text.empty() && input.display_text != message.content) {
        message.metadata["display_text"] = std::move(input.display_text);
    }
    message.metadata["turn_steer"] = true;
    message.metadata["turn_id"] = turn_id;
    ensure_user_message_identity(message);

    history_.append(message);
    if (session) {
        session->on_message(message);
    }
    emit_session_summary_updated(session);

    const std::string display = message.metadata.value(
        "display_text", message.content);
    if (callbacks.on_message) {
        callbacks.on_message("user", display, false);
    }

    nlohmann::json event = {
        {"role", "user"},
        {"content", message.content},
        {"is_tool", false},
        {"id", message.uuid},
        {"metadata", message.metadata},
    };
    if (message.content_parts.is_array() && !message.content_parts.empty()) {
        event["content_parts"] = message.content_parts;
    }
    events_.emit(SessionEventKind::Message, std::move(event));
    LOG_INFO("[turn/steer] committed input to active turn " + turn_id);
}

} // namespace acecode::agent
