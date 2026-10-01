#include "conversation_history.hpp"

#include "session/session_client.hpp"
#include "session/thread_repair.hpp"
#include "session/tool_result_storage.hpp"
#include "utils/uuid.hpp"
#include "utils/logger.hpp"

#include <utility>

namespace acecode::agent {

void ConversationHistory::warn_unless_idle_access(bool worker_or_queue_held) const {
    if (busy_.load() || !worker_or_queue_held) {
        LOG_WARN("ConversationHistory idle mutation requires an idle worker or held queue gate");
    }
}

void ConversationHistory::remember_tool_call_ids(const ChatMessage& message) {
    if (!message.tool_call_id.empty()) used_tool_call_ids_.insert(message.tool_call_id);
    if (message.tool_calls.is_array()) {
        for (const auto& call : message.tool_calls) {
            if (!call.is_object()) continue;
            const auto id = call.find("id");
            if (id != call.end() && id->is_string() && !id->get_ref<const std::string&>().empty()) {
                used_tool_call_ids_.insert(id->get<std::string>());
            }
        }
    }
    for (const auto& record : decode_content_replacement_message(message)) {
        used_tool_call_ids_.insert(record.tool_call_id);
    }
}

void ConversationHistory::prepare_tool_calls(std::vector<ToolCall>& calls) {
    std::unordered_set<std::string> batch_ids;
    for (const auto& call : calls) batch_ids.insert(call.id);
    for (auto& call : calls) {
        if (!call.id.empty() && used_tool_call_ids_.insert(call.id).second) continue;
        do {
            call.id = "call_ace_" + generate_uuid_v7();
        } while (batch_ids.count(call.id) || !used_tool_call_ids_.insert(call.id).second);
    }
}

void ConversationHistory::append(ChatMessage message) {
    remember_tool_call_ids(message);
    messages_.push_back(std::move(message));
}

void ConversationHistory::replace(std::vector<ChatMessage> messages) {
    for (const auto& message : messages) remember_tool_call_ids(message);
    messages_ = std::move(messages);
}

void ConversationHistory::clear() {
    messages_.clear();
}

void ConversationHistory::restore(ChatMessage message, bool worker_or_queue_held) {
    warn_unless_idle_access(worker_or_queue_held);
    append(std::move(message));
}

void ConversationHistory::clear_idle(bool worker_or_queue_held) {
    warn_unless_idle_access(worker_or_queue_held);
    clear();
    live_tail_blocked_ = false;
}

void ConversationHistory::on_worker(
    const std::function<void(ConversationHistory&)>& operation, bool worker_or_queue_held) {
    warn_unless_idle_access(worker_or_queue_held);
    if (operation) operation(*this);
}

ThreadRepairResult ConversationHistory::repair(
    SessionManager* session, const ThreadRepairOptions& options) {
    auto result = apply_thread_repair(session, messages_, options);
    for (const auto& message : messages_) remember_tool_call_ids(message);
    return result;
}

void ConversationHistory::observe_transcript(const SessionEvent& event) {
    switch (event.kind) {
    case SessionEventKind::Message: {
        const auto& payload = event.payload;
        const auto metadata = payload.value("metadata", nlohmann::json::object());
        if (!payload.value("is_meta", false) &&
            !(metadata.is_object() && metadata.value("hidden_goal_context", false))) {
            const auto role = payload.value("role", std::string{});
            const bool user_abort = role == "system" && metadata.is_object() &&
                metadata.value("transcript_only", false) &&
                metadata.value("user_aborted", false);
            live_tail_blocked_ = role != "user" && !user_abort;
        }
        break;
    }
    case SessionEventKind::Token:
    case SessionEventKind::Reasoning:
        if (!event.payload.value("text", std::string{}).empty()) {
            live_tail_blocked_ = true;
        }
        break;
    case SessionEventKind::ToolStart:
    case SessionEventKind::ToolUpdate:
    case SessionEventKind::ToolEnd:
    case SessionEventKind::Error:
        live_tail_blocked_ = true;
        break;
    case SessionEventKind::TranscriptReplace:
        // A full replacement discards transient output. The canonical
        // histories are still checked before any retry is accepted.
        live_tail_blocked_ = false;
        break;
    default:
        break;
    }
}

} // namespace acecode::agent
