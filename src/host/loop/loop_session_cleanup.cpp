#include "loop_session_cleanup.hpp"

#include "session/session_client.hpp"
#include "session/session_pin_store.hpp"
#include "session/session_storage.hpp"
#include "session/session_tree_purge.hpp"
#include "session_host/session_registry.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace acecode::loop {
namespace {

bool session_busy(SessionRegistry& registry, const std::string& id) {
    const auto entry = registry.acquire(id);
    return entry && entry->loop && entry->loop->is_busy();
}

bool archive_session(const std::string& project_dir,
                     const std::string& id,
                     SessionRegistry& registry,
                     SessionClient& client) {
    // 与 Web 归档同一顺序:先销毁内存会话(落盘并释放写锁),再改磁盘 meta。
    if (registry.acquire(id)) client.destroy_session(id);
    const auto path = SessionStorage::meta_path(project_dir, id);
    auto meta = SessionStorage::read_meta(path);
    if (meta.id.empty() || meta.archived) return false;
    meta.archived = true;
    return SessionStorage::write_meta(path, meta);
}

bool delete_session_tree(const std::string& project_dir,
                         const std::string& id,
                         SessionRegistry& registry,
                         SessionClient& client) {
    auto doomed = session_tree_delete_order(project_dir, id);
    // 还没落盘、只在内存里的子会话也要一起停掉。
    for (const auto& info : client.list_sessions()) {
        if (info.parent_session_id == id &&
            std::find(doomed.begin(), doomed.end(), info.id) == doomed.end()) {
            doomed.insert(doomed.begin(), info.id);
        }
    }
    for (const auto& target : doomed) {
        if (session_busy(registry, target)) return false;
    }
    for (const auto& target : doomed) {
        if (registry.acquire(target)) client.destroy_session(target);
    }
    std::string error;
    if (!purge_session_tree(project_dir, id, &error)) {
        LOG_WARN("[loop] failed to delete old session " + id + ": " + error);
        return false;
    }
    return true;
}

} // namespace

SessionCleanupMode session_cleanup_mode(const LoopDefinition& loop) {
    if (loop.auto_delete_old_sessions) return SessionCleanupMode::Delete;
    if (loop.auto_archive_old_sessions) return SessionCleanupMode::Archive;
    return SessionCleanupMode::Off;
}

std::vector<std::string> select_old_loop_sessions(
    const std::vector<LoopSessionState>& newest_first, int keep) {
    std::vector<std::string> result;
    int kept = 0;
    for (const auto& session : newest_first) {
        if (!session.visible) continue;
        if (kept < keep) {
            ++kept;
            continue;
        }
        if (!session.busy) result.push_back(session.session_id);
    }
    return result;
}

LoopSessionStorage loop_session_storage(const LoopDefinition& loop,
                                        const std::string& no_workspace_cache_root) {
    LoopSessionStorage storage;
    if (!loop.workspace_cwd.empty()) {
        const auto project_dir = SessionStorage::get_project_dir(loop.workspace_cwd);
        storage.project_dir = [project_dir](const std::string&) { return project_dir; };
        storage.pinned_sessions_path = path_from_utf8(project_dir) / "pinned_sessions.json";
        return storage;
    }
    const std::string root = no_workspace_cache_root.empty()
        ? default_no_workspace_cache_root() : no_workspace_cache_root;
    storage.project_dir = [root](const std::string& session_id) {
        return SessionStorage::get_project_dir(no_workspace_session_cwd(session_id, root));
    };
    storage.pinned_sessions_path = path_from_utf8(root) / "pinned_sessions.json";
    return storage;
}

LoopSessionCleanupResult cleanup_old_loop_sessions(
    const LoopDefinition& loop,
    const std::vector<std::string>& session_ids_newest_first,
    const LoopSessionStorage& storage,
    SessionRegistry& registry,
    SessionClient& client) {
    LoopSessionCleanupResult result;
    const auto mode = session_cleanup_mode(loop);
    if (mode == SessionCleanupMode::Off || !storage.project_dir) return result;

    const auto pins = session_pins::read_pinned_sessions_state(storage.pinned_sessions_path);
    const std::set<std::string> pinned(pins.session_ids.begin(), pins.session_ids.end());
    std::vector<LoopSessionState> states;
    states.reserve(session_ids_newest_first.size());
    for (const auto& id : session_ids_newest_first) {
        LoopSessionState state;
        state.session_id = id;
        const auto entry = registry.acquire(id);
        state.busy = entry && entry->loop && entry->loop->is_busy();
        const auto meta = SessionStorage::read_meta(
            SessionStorage::meta_path(storage.project_dir(id), id));
        // 刚建好的会话可能还没落盘,但已经在侧栏里了。
        const bool exists = !meta.id.empty() || entry != nullptr;
        state.visible = exists && !meta.archived && meta.parent_session_id.empty() &&
                        pinned.count(id) == 0;
        states.push_back(std::move(state));
    }

    for (const auto& id : select_old_loop_sessions(states, loop.keep_recent_sessions)) {
        const auto project_dir = storage.project_dir(id);
        if (mode == SessionCleanupMode::Archive) {
            if (archive_session(project_dir, id, registry, client)) result.archived.push_back(id);
        } else if (delete_session_tree(project_dir, id, registry, client)) {
            result.deleted.push_back(id);
        }
    }
    if (!result.archived.empty() || !result.deleted.empty()) {
        LOG_INFO("[loop] task " + loop.id + " kept " +
                 std::to_string(loop.keep_recent_sessions) + " recent session(s), archived " +
                 std::to_string(result.archived.size()) + ", deleted " +
                 std::to_string(result.deleted.size()));
    }
    return result;
}

} // namespace acecode::loop
