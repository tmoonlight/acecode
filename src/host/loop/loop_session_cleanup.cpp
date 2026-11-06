#include "loop_session_cleanup.hpp"

#include "session/session_client.hpp"
#include "session/session_pin_store.hpp"
#include "session/session_storage.hpp"
#include "session/session_tree_purge.hpp"
#include "session_host/session_registry.hpp"
#include "session/session_writer_lease.hpp"
#include "utils/logger.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace acecode::loop {
namespace {

bool cleanup_session(const LoopSessionStorage& storage, const std::string& loop_id,
                     SessionCleanupMode mode, SessionRegistry& registry, SessionClient& client) {
    const auto& id = storage.id;
    const auto& dir = storage.project_dir;
    auto targets = mode == SessionCleanupMode::Delete
        ? session_tree_delete_order(dir, id) : std::vector<std::string>{id};
    const std::unordered_set<std::string> target_ids(targets.begin(), targets.end());
    if (mode == SessionCleanupMode::Delete) {
        // A not-yet-persisted child can still be starting. Defer the entire tree.
        for (const auto& info : client.list_sessions()) {
            if (target_ids.count(info.parent_session_id) && !target_ids.count(info.id)) return false;
        }
    }
    for (const auto& target : targets) {
        const auto entry = registry.acquire(target);
        if (entry && (!entry->loop || entry->loop->has_pending_work())) return false;
    }

    std::vector<WriterLease> leases;
    for (const auto& target : targets) {
        if (const auto entry = registry.acquire(target)) {
            if (!entry->sm || entry->sm->current_project_dir() != dir ||
                !entry->loop || !entry->loop->close_if_idle()) return false;
            if (!registry.destroy(target, entry.get())) return false;
        }
        leases.emplace_back(dir, target);
        if (!leases.back().acquire_for_cleanup()) return false;
    }

    const auto pins = session_pins::read_pinned_sessions_state(storage.pinned_sessions_path);
    for (const auto& pinned : pins.session_ids) {
        if (target_ids.count(pinned)) return false;
    }
    const auto path = SessionStorage::meta_path(dir, id);
    auto meta = SessionStorage::read_meta(path);
    if (meta.id != id || meta.loop_id != loop_id || meta.archived || !meta.parent_session_id.empty()) return false;
    if (mode == SessionCleanupMode::Archive) {
        meta.archived = true;
        return SessionStorage::write_meta(path, meta);
    }

    // Only purge the exact set we own. A newly discovered child defers the tree.
    const auto order = session_tree_delete_order(dir, id);
    if (order.size() != target_ids.size() ||
        std::any_of(order.begin(), order.end(), [&](const auto& child) { return !target_ids.count(child); })) {
        return false;
    }
    std::string error;
    if (purge_sessions_in_order(dir, order, &error)) return true;
    LOG_WARN("[loop] failed to delete old session " + id + ": " + error);
    return false;
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

std::vector<LoopSessionStorage> loop_session_storage(const LoopDefinition& loop,
    const std::vector<LoopRunSession>& runs, const std::string& no_workspace_cache_root) {
    namespace fs = std::filesystem;
    const auto root = no_workspace_cache_root.empty() ? default_no_workspace_cache_root() : no_workspace_cache_root;
    std::vector<LoopSessionStorage> result;
    std::optional<std::vector<fs::path>> legacy_projects;
    for (const auto& run : runs) {
        const auto& cwd = run.workspace_cwd ? *run.workspace_cwd : loop.workspace_cwd;
        bool no_workspace = cwd.empty();
        auto dir = SessionStorage::get_project_dir(no_workspace ? no_workspace_session_cwd(run.id, root) : cwd);
        std::error_code lookup_error;
        if (!run.workspace_cwd && !fs::exists(path_from_utf8(SessionStorage::meta_path(dir, run.id)), lookup_error)) {
            // Pre-upgrade rows lack a scope snapshot. Probe metadata by the exact ID,
            // and verify LOOP provenance before considering another project's files.
            if (!legacy_projects) {
                legacy_projects.emplace();
                std::error_code error;
                const auto projects = path_from_utf8(get_acecode_dir()) / "projects";
                for (fs::directory_iterator it(projects, error), end; !error && it != end; it.increment(error)) {
                    if (it->is_directory(error)) legacy_projects->push_back(it->path());
                }
            }
            for (const auto& project : *legacy_projects) {
                const auto candidate = path_to_utf8(project);
                const auto meta = SessionStorage::read_meta(SessionStorage::meta_path(candidate, run.id));
                if (meta.id != run.id || meta.loop_id != loop.id) continue;
                dir = candidate;
                no_workspace = meta.no_workspace;
                break;
            }
        }
        result.push_back({run.id, dir, path_from_utf8(no_workspace ? root : dir) / "pinned_sessions.json"});
    }
    return result;
}

LoopSessionCleanupResult cleanup_old_loop_sessions(
    const LoopDefinition& loop, const std::vector<LoopSessionStorage>& sessions,
    SessionRegistry& registry, SessionClient& client) {
    LoopSessionCleanupResult result;
    const auto mode = session_cleanup_mode(loop);
    if (mode == SessionCleanupMode::Off) return result;

    std::vector<LoopSessionState> states;
    std::map<std::filesystem::path, std::set<std::string>> pins_by_scope;
    std::unordered_map<std::string, const LoopSessionStorage*> locations;
    for (const auto& storage : sessions) {
        auto [pins, inserted] = pins_by_scope.try_emplace(storage.pinned_sessions_path);
        if (inserted) {
            const auto saved = session_pins::read_pinned_sessions_state(storage.pinned_sessions_path);
            pins->second.insert(saved.session_ids.begin(), saved.session_ids.end());
        }
        const auto& id = storage.id;
        const auto entry = registry.acquire(id);
        const auto meta = SessionStorage::read_meta(SessionStorage::meta_path(storage.project_dir, id));
        const bool visible = (!meta.id.empty() ? meta.id == id && meta.loop_id == loop.id : entry != nullptr)
            && !meta.archived && meta.parent_session_id.empty() && !pins->second.count(id);
        states.push_back({id, visible, entry && entry->loop && entry->loop->has_pending_work()});
        locations.emplace(id, &storage);
    }
    for (const auto& id : select_old_loop_sessions(states, loop.keep_recent_sessions)) {
        if (cleanup_session(*locations.at(id), loop.id, mode, registry, client)) {
            (mode == SessionCleanupMode::Archive ? result.archived : result.deleted).push_back(id);
        }
    }
    return result;
}

} // namespace acecode::loop
