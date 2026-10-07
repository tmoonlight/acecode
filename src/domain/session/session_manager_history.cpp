#include "session_manager.hpp"
#include "session_recent_activity.hpp"
#include "turn_timing.hpp"
#include "session_history_page.hpp"
#include "utils/logger.hpp"
#include <filesystem>

namespace acecode {
namespace fs = std::filesystem;

void SessionManager::on_message(const ChatMessage& msg) {
    (void)try_on_message(msg);
}

void SessionManager::record_turn_outcome(const std::string& outcome) {
    if (!is_valid_turn_timing_status(outcome)) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (last_turn_outcome_ == outcome) return;
    last_turn_outcome_ = outcome;
    update_meta();
}

void SessionManager::record_recent_activity_locked(const ChatMessage& msg) {
    if (is_recent_user_message(msg)) {
        last_user_message_at_ = msg.timestamp.empty() ? SessionStorage::now_iso8601() : msg.timestamp;
        last_turn_outcome_.clear();
    } else if (is_turn_timing_message(msg)) {
        last_turn_outcome_ = decode_turn_timing(msg.metadata.at("turn_timing"))->status;
    }
}

void SessionManager::restore_recent_activity_locked(const SessionMeta& persisted_meta) {
    last_user_message_at_ = persisted_meta.last_user_message_at;
    last_turn_outcome_ = persisted_meta.last_turn_outcome;
    if (last_user_message_at_.empty()) {
        const auto recent = read_session_recent_activity(jsonl_path_);
        last_user_message_at_ = recent.last_user_message_at;
        if (last_turn_outcome_.empty()) last_turn_outcome_ = recent.last_turn_outcome;
    }
}

void SessionManager::publish_display_snapshot_locked() {
    SessionDisplaySnapshot snapshot{
        pending_title_, title_source_, last_user_summary_, turn_count_,
        last_token_usage_, session_token_usage_, worktree_, swarm_mode_, agent_path_,
        last_user_message_at_, last_turn_outcome_,
    };
    std::lock_guard<std::mutex> lock(display_mu_);
    display_snapshot_ = std::move(snapshot);
}

std::string SessionManager::current_summary() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_user_summary_;
}

SessionDisplaySnapshot SessionManager::display_snapshot() const {
    std::lock_guard<std::mutex> lock(display_mu_);
    return display_snapshot_;
}

bool SessionManager::file_checkpoint_can_restore(const std::string& user_message_uuid) const {
    std::lock_guard<std::mutex> lk(mu_);
    ensure_file_checkpoints_loaded_locked();
    return checkpoint_store_.can_restore(user_message_uuid);
}

FileCheckpointDiffStats SessionManager::file_checkpoint_diff_stats(
    const std::string& user_message_uuid) const {
    std::lock_guard<std::mutex> lk(mu_);
    ensure_file_checkpoints_loaded_locked();
    return checkpoint_store_.diff_stats(user_message_uuid);
}

FileCheckpointRestoreResult SessionManager::rewind_files_to_checkpoint(
    const std::string& user_message_uuid) const {
    std::lock_guard<std::mutex> lk(mu_);
    ensure_file_checkpoints_loaded_locked();
    return checkpoint_store_.rewind_to(user_message_uuid);
}

void SessionManager::ensure_file_checkpoints_loaded_locked() const {
    if (file_checkpoints_loaded_) return;
    checkpoint_store_.load_from_messages(project_dir_, session_id_,
                                         load_session_file_checkpoints(jsonl_path_));
    file_checkpoints_loaded_ = true;
}

std::optional<CompactCheckpoint> SessionManager::load_latest_compact_checkpoint() const {
    for (int attempt = 0; attempt < 2; ++attempt) {
        SessionFileReader reader("");
        {
            std::lock_guard<std::mutex> lk(mu_);
            reader = SessionFileReader(jsonl_path_);
        }
        if (!reader.valid()) return std::nullopt;
        const auto prefix = reader.prefix();
        auto checkpoint = latest_compact_checkpoint(reader);
        if (reader.unchanged(prefix)) {
            return checkpoint ? std::optional<CompactCheckpoint>(std::move(checkpoint->checkpoint)) : std::nullopt;
        }
    }
    throw HistorySnapshotChanged();
}

std::vector<ChatMessage> SessionManager::load_active_messages() const {
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            std::optional<SessionFileReader> reader;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (!started_ || !created_ || jsonl_path_.empty()) return {};
                // Capture a stable native handle and its size, without reading
                // content. FILE_SHARE_DELETE lets atomic rewrites proceed.
                reader.emplace(jsonl_path_);
            }
            if (!reader->valid()) return {};
            const auto prefix = reader->prefix();
            auto result = SessionStorage::load_messages_snapshot(*reader);
            if (reader->unchanged(prefix)) return std::move(result.messages);
        } catch (const HistorySnapshotChanged&) {
            if (attempt == 1) throw;
        }
    }
    throw HistorySnapshotChanged();
}

SessionMeta SessionManager::load_session_meta(const std::string& session_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (project_dir_.empty()) return {};
    const auto meta_path = SessionStorage::meta_path(project_dir_, session_id);
    if (!fs::exists(meta_path)) {
        if (SessionStorage::has_incompatible_pid_session_files(project_dir_, session_id)) {
            LOG_WARN("[session] meta for " + session_id +
                     " not loaded: incompatible PID-suffixed old data is unsupported");
        }
        return {};
    }
    return SessionStorage::read_meta(meta_path);
}

bool SessionManager::has_session_file(const std::string& session_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (project_dir_.empty() || session_id.empty()) return false;
    const auto candidates = SessionStorage::find_session_files(project_dir_, session_id);
    return !candidates.empty();
}

bool SessionManager::has_incompatible_session_data(const std::string& session_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (project_dir_.empty()) return false;
    return SessionStorage::has_incompatible_pid_session_files(project_dir_, session_id);
}

} // namespace acecode
