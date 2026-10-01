// SessionManager 的会话级开关:归档状态与记忆开关。都只改一个字段并在会话已
// 落盘时立即写 meta,单独成文件让 session_manager.cpp 保持在行数基线内。
#include "session_manager.hpp"

namespace acecode {

void SessionManager::set_session_archived(bool archived) {
    std::lock_guard<std::mutex> lk(mu_);
    archived_ = archived;
    if (created_) {
        update_meta();
    }
}

ArchiveCurrentSessionResult SessionManager::archive_current_session() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!created_ || finalized_ || session_id_.empty()) {
        return ArchiveCurrentSessionResult::NoActiveSession;
    }

    const bool previous_archived = archived_;
    archived_ = true;
    if (!update_meta()) {
        archived_ = previous_archived;
        last_error_ = "Failed to persist archived session metadata.";
        return ArchiveCurrentSessionResult::PersistenceFailed;
    }

    last_error_.clear();
    return ArchiveCurrentSessionResult::Archived;
}

bool SessionManager::is_no_workspace() const {
    std::lock_guard<std::mutex> lk(mu_);
    return no_workspace_;
}

void SessionManager::set_memory_enabled(bool enabled) {
    std::lock_guard<std::mutex> lk(mu_);
    memory_mode_ = enabled ? std::string{} : std::string("off");
    // 会话还没落盘时只记在内存里,第一次写 meta 时一并带上。
    if (created_) {
        update_meta();
    }
}

bool SessionManager::memory_enabled() const {
    std::lock_guard<std::mutex> lk(mu_);
    return memory_mode_ != "off";
}

} // namespace acecode
