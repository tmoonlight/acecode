#include "agent/agent_loop.hpp"
#include "agent/event_payload/message_payload.hpp"
#include "hooks/hook_runtime.hpp"
#include "llm/tool_protocol_names.hpp"
#include "permissions/shell_write_guard.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_rewind.hpp"
#include "session/session_storage.hpp"
#include "session/turn_timing.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"
#include "utils/uuid.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode {

void AgentLoop::dispatch_message(const std::string& role,
                                  const std::string& content,
                                  bool is_tool,
                                  nlohmann::json metadata,
                                  nlohmann::json content_parts) {
    if (role == "error") {
        // 回合级错误文案的唯一收集点:provider 终止错误 / 压缩失败 / 空回复
        // 耗尽 / hook 拦截都经这里派发,wait_subagent 报 ChildFailed 时带上。
        std::lock_guard<std::mutex> lk(last_turn_error_mu_);
        last_turn_error_ = content;
    }
    if (callbacks_.on_message) {
        callbacks_.on_message(role, content, is_tool);
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

void AgentLoop::append_turn_timing_record(const std::string& user_message_uuid,
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
    messages_.push_back(msg);
    if (session_manager_) {
        session_manager_->on_message(msg);
        session_manager_->record_trajectory_event(
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

void AgentLoop::append_tool_user_prompt(const std::string& content,
                                        const std::string& display_text,
                                        const std::string& source_tool) {
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

    messages_.push_back(msg);
    if (session_manager_) {
        session_manager_->on_message(msg);
    }

    if (callbacks_.on_message) {
        callbacks_.on_message("user", msg.metadata.value("display_text", msg.content), false);
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

void AgentLoop::emit_system_message(const std::string& content, nlohmann::json metadata) {
    dispatch_message("system", content, false, std::move(metadata));
}

void AgentLoop::emit_transcript_system_message(const std::string& content,
                                               nlohmann::json metadata) {
    ChatMessage msg;
    msg.role = "system";
    msg.content = content;
    msg.timestamp = SessionStorage::now_iso8601();
    msg.metadata = metadata.is_object() ? std::move(metadata) : nlohmann::json::object();
    msg.metadata["transcript_only"] = true;

    if (callbacks_.on_transcript_message) {
        callbacks_.on_transcript_message(msg);
    } else if (callbacks_.on_message) {
        callbacks_.on_message(msg.role, msg.content, false);
    }
    if (session_manager_) {
        session_manager_->on_message(msg);
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

void AgentLoop::inject_shell_turn(const std::string& cmd,
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
    messages_.push_back(std::move(msg));
}

} // namespace acecode
