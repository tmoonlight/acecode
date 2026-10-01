#include "compaction_controller.hpp"
#include "compact.hpp"
#include "agent/agent_callbacks.hpp"
#include "agent/boundary/workspace_boundary.hpp"
#include "agent/hook_bridge/agent_hook_bridge.hpp"
#include "agent/model_step/active_model_view.hpp"
#include "agent/model_step/active_provider_slot.hpp"
#include "agent/progress/retry_progress.hpp"
#include "agent/request/api_request_builder.hpp"
#include "agent/request/provider_history.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/transcript/trajectory_recorder.hpp"
#include "agent/turn/busy_cycle.hpp"
#include "session/compact_checkpoint.hpp"
#include "session/compact_notice.hpp"
#include "session/event_dispatcher.hpp"
#include "session/session_manager.hpp"
#include "session/system_notice.hpp"
#include "session/task_suggestion_store.hpp"
#include "session/thread_repair.hpp"
#include "session/token_tracker.hpp"
#include "tool/mtime_tracker.hpp"
#include "utils/abort_signal.hpp"
#include "utils/logger.hpp"
#include "utils/encoding.hpp"
#include "utils/time.hpp"
#include "utils/uuid.hpp"
#include <algorithm>
#include <limits>
#include <utility>

namespace acecode::agent {
using detail::recovered_provider_messages;
using utils::now_epoch_ms;

bool CompactionController::exceeds_auto_threshold(
    const CompactionInputs& inputs,
    const UserInput* pending_input) const {
    auto request = requests_.initial_context(inputs.request);
    auto history = recovered_provider_messages(history_.view(), "token-estimate");
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
        ActiveModelView(inputs.provider, inputs.request.context_window, environment_).effective_window(),
        last_api_total_tokens_.load(std::memory_order_relaxed),
        estimate_message_tokens(request));
}

void CompactionController::initialize_window(const CompactionInputs& inputs) {
    if (compact_window_initialized_) return;
    compact_window_initialized_ = true;

    compact_window_number_ = 0;
    compact_current_window_id_ = generate_uuid_v7();
    compact_first_window_id_ = compact_current_window_id_;

    if (!inputs.session) return;
    if (const auto checkpoint = inputs.session->load_latest_compact_checkpoint()) {

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

void CompactionController::apply_result(
    const CompactionInputs& inputs,
    const CompactResult& result,
    const std::string& trigger,
    const std::string& compact_notice_id) {
    auto initial_context = requests_.initial_context(inputs.request);
    auto pre_history = recovered_provider_messages(history_.view(), "compact-input");
    auto pre_request = initial_context;
    pre_request.insert(pre_request.end(), pre_history.begin(), pre_history.end());
    const int pre_tokens = estimate_message_tokens(pre_request);
    std::vector<ChatMessage> replacement_history =
        recovered_provider_messages(result.compacted_messages, "compact-output");
    auto post_request = initial_context;
    post_request.insert(
        post_request.end(), replacement_history.begin(), replacement_history.end());
    const int post_tokens = estimate_message_tokens(post_request);

    initialize_window(inputs);
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
    if (inputs.session) {
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
        checkpoint_persisted = inputs.session->append_compact_checkpoint(checkpoint);
    }
    history_.replace(std::move(replacement_history));
    last_api_total_tokens_.store(post_tokens, std::memory_order_relaxed);
    environment_.mtime_tracker().clear_read_observations();
    compact_generation_.fetch_add(1, std::memory_order_relaxed);

    const std::string notice_id = compact_notice_id.empty()
        ? generate_uuid_v7()
        : compact_notice_id;
    transcript_.emit_transcript_system_message(inputs.session,
        "--- [Compact Checkpoint] ---",
        make_compact_notice_metadata(notice_id, "checkpoint"));
    transcript_.emit_transcript_system_message(inputs.session,
        "[Conversation summary]\n" + result.summary_text,
        make_compact_notice_metadata(notice_id, "summary", true, {{"summary", result.summary_text}}));
    if (checkpoint_persisted) {
        const auto project_dir = inputs.session->current_project_dir();
        const auto source_id = inputs.session->current_session_id();
        const int threshold = inputs.suggestion_threshold;
        if (!project_dir.empty() && !source_id.empty() && threshold > 0) {
            TaskSuggestionStore store(path_from_utf8(project_dir));
            std::string error;
            store.propose_continuation(
                source_id, inputs.session->load_active_messages(),
                static_cast<std::uint64_t>(threshold),
                {{"source_working_cwd", boundary_.cwd()}}, &error);
            if (!error.empty()) LOG_WARN("[task-suggestion] " + error);
        }
    }
}


void CompactionController::reset_window() {
    compact_window_initialized_ = false;
    compact_window_number_ = 0;
    compact_first_window_id_.clear();
    compact_current_window_id_.clear();
}

} // namespace acecode::agent
