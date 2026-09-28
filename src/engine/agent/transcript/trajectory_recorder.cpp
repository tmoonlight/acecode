#include "agent/agent_loop.hpp"
#include "agent/approval/permission_payloads.hpp"
#include "agent/request/request_context.hpp"
#include "agent/transcript/transcript_queries.hpp"
#include "hooks/hook_runtime.hpp"
#include "pa/pa_overflow_rescue.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/thread_goal_store.hpp"
#include "session/thread_repair.hpp"
#include "session/turn_timing.hpp"
#include "utils/logger.hpp"
#include "utils/time.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode {

using agent::detail::should_persist_trajectory_event;
using utils::now_epoch_ms;

void AgentLoop::set_session_manager(SessionManager* sm) {
    session_manager_ = sm;
    if (!sm) {
        events_.set_observer({});
        return;
    }
    events_.set_observer([this, sm](const SessionEvent& event) {
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
                live_transcript_tail_blocked_ = role != "user" && !user_abort;
            }
            break;
        }
        case SessionEventKind::Token:
        case SessionEventKind::Reasoning:
            if (!event.payload.value("text", std::string{}).empty()) {
                live_transcript_tail_blocked_ = true;
            }
            break;
        case SessionEventKind::ToolStart:
        case SessionEventKind::ToolUpdate:
        case SessionEventKind::ToolEnd:
        case SessionEventKind::Error:
            live_transcript_tail_blocked_ = true;
            break;
        case SessionEventKind::TranscriptReplace:
            // A full replacement discards transient output. The canonical
            // histories are still checked before any retry is accepted.
            live_transcript_tail_blocked_ = false;
            break;
        default:
            break;
        }
        if (!should_persist_trajectory_event(event)) return;
        sm->record_trajectory_event(
            to_string(event.kind), event.payload, event.timestamp_ms);
    });
}

void AgentLoop::record_terminal_trajectory_events(
    nlohmann::json busy_payload,
    nlohmann::json done_payload) {
    if (!session_manager_) return;
    const std::int64_t timestamp_ms = now_epoch_ms();
    session_manager_->record_trajectory_event(
        "busy_changed", std::move(busy_payload), timestamp_ms);
    session_manager_->record_trajectory_event(
        "done", std::move(done_payload), timestamp_ms);
}

} // namespace acecode
