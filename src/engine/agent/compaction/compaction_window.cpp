#include "agent/agent_loop.hpp"
#include "agent/approval/permission_payloads.hpp"
#include "agent/compaction/compact.hpp"
#include "agent/request/provider_history.hpp"
#include "agent/request/request_context.hpp"
#include "agent/transcript/transcript_queries.hpp"
#include "llm/tool_protocol_names.hpp"
#include "permissions/shell_write_guard.hpp"
#include "session/compact_checkpoint.hpp"
#include "session/compact_notice.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/task_suggestion_store.hpp"
#include "session/turn_timing.hpp"
#include "tool/mtime_tracker.hpp"
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

using agent::detail::recovered_provider_messages;

bool AgentLoop::active_estimate_exceeds_auto_threshold(
    const UserInput* pending_input) const {
    auto request = build_compaction_initial_context();
    auto history = recovered_provider_messages(messages_, "token-estimate");
    if (pending_input && !pending_input->empty()) {
        ChatMessage pending;
        pending.role = "user";
        pending.content = pending_input->text;
        pending.content_parts = pending_input->content_parts;
        pending.metadata = pending_input->metadata;
        history.push_back(std::move(pending));
    }
    request.insert(request.end(), history.begin(), history.end());
    return should_auto_compact(
        compaction_context_window(),
        last_api_total_tokens_.load(std::memory_order_relaxed),
        estimate_message_tokens(request));
}

void AgentLoop::initialize_compact_window_state() {
    if (compact_window_initialized_) return;
    compact_window_initialized_ = true;

    compact_window_number_ = 0;
    compact_current_window_id_ = generate_uuid_v7();
    compact_first_window_id_ = compact_current_window_id_;

    if (!session_manager_) return;
    const auto raw_messages = session_manager_->load_active_messages();
    for (auto it = raw_messages.rbegin(); it != raw_messages.rend(); ++it) {
        const auto checkpoint = decode_compact_checkpoint(*it);
        if (!checkpoint.has_value()) continue;

        compact_window_number_ = checkpoint->window_number;
        if (!checkpoint->window_id.empty()) {
            compact_current_window_id_ = checkpoint->window_id;
        } else if (!checkpoint->id.empty()) {
            // A v1 checkpoint predates explicit window IDs. Its checkpoint ID
            // is a stable legacy epoch identity for the next transition.
            compact_current_window_id_ = checkpoint->id;
        }
        compact_first_window_id_ = checkpoint->first_window_id.empty()
            ? compact_current_window_id_
            : checkpoint->first_window_id;
        return;
    }
}

void AgentLoop::apply_compact_result(
    const CompactResult& result,
    const std::string& trigger,
    const std::string& compact_notice_id) {
    auto initial_context = build_compaction_initial_context();
    auto pre_history = recovered_provider_messages(messages_, "compact-input");
    auto pre_request = initial_context;
    pre_request.insert(pre_request.end(), pre_history.begin(), pre_history.end());
    const int pre_tokens = estimate_message_tokens(pre_request);
    std::vector<ChatMessage> replacement_history =
        recovered_provider_messages(result.compacted_messages, "compact-output");
    auto post_request = initial_context;
    post_request.insert(
        post_request.end(), replacement_history.begin(), replacement_history.end());
    const int post_tokens = estimate_message_tokens(post_request);

    initialize_compact_window_state();
    const std::string previous_window_id = compact_current_window_id_;
    if (compact_window_number_ <
        std::numeric_limits<std::uint64_t>::max()) {
        ++compact_window_number_;
    }
    compact_current_window_id_ = generate_uuid_v7();
    if (compact_first_window_id_.empty()) {
        compact_first_window_id_ = previous_window_id.empty()
            ? compact_current_window_id_
            : previous_window_id;
    }

    bool checkpoint_persisted = false;
    if (session_manager_) {
        CompactCheckpoint checkpoint;
        checkpoint.trigger = trigger;
        checkpoint.summary = result.summary_text;
        checkpoint.messages_compressed = result.messages_compressed;
        checkpoint.estimated_tokens_saved = result.estimated_tokens_saved;
        checkpoint.pre_tokens = pre_tokens;
        checkpoint.post_tokens = post_tokens;
        checkpoint.window_number = compact_window_number_;
        checkpoint.first_window_id = compact_first_window_id_;
        checkpoint.previous_window_id = previous_window_id;
        checkpoint.window_id = compact_current_window_id_;
        checkpoint.replacement_history = replacement_history;
        checkpoint_persisted = session_manager_->append_compact_checkpoint(checkpoint);
    }
    messages_ = std::move(replacement_history);
    last_api_total_tokens_.store(post_tokens, std::memory_order_relaxed);
    MtimeTracker::instance().clear_read_observations();
    compact_generation_.fetch_add(1, std::memory_order_relaxed);

    const std::string notice_id = compact_notice_id.empty()
        ? generate_uuid_v7()
        : compact_notice_id;
    emit_transcript_system_message(
        "--- [Compact Checkpoint] ---",
        make_compact_notice_metadata(notice_id, "checkpoint"));
    emit_transcript_system_message(
        "[Conversation summary]\n" + result.summary_text,
        make_compact_notice_metadata(notice_id, "summary", true, {{"summary", result.summary_text}}));
    if (checkpoint_persisted) {
        const auto project_dir = session_manager_->current_project_dir();
        const auto source_id = session_manager_->current_session_id();
        const int threshold = task_suggestion_compact_threshold_.load(std::memory_order_relaxed);
        if (!project_dir.empty() && !source_id.empty() && threshold > 0) {
            TaskSuggestionStore store(path_from_utf8(project_dir));
            std::string error;
            store.propose_continuation(
                source_id, session_manager_->load_active_messages(),
                static_cast<std::uint64_t>(threshold),
                {{"source_working_cwd", cwd_}}, &error);
            if (!error.empty()) LOG_WARN("[task-suggestion] " + error);
        }
    }
}

} // namespace acecode
