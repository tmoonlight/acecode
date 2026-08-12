#include "rc_session_catalog.hpp"

#include "desktop/workspace_registry.hpp"
#include "session/session_registry.hpp"
#include "session/session_storage.hpp"
#include "session/session_user_message_search.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace acecode::rc {
namespace {

namespace fs = std::filesystem;

struct WorkspaceSnapshot {
    std::string hash;
    std::string cwd;
    std::string name;
};

WorkspaceSnapshot workspace_snapshot(const fs::path& project_dir) {
    WorkspaceSnapshot out;
    out.hash = path_to_utf8(project_dir.filename());
    const fs::path workspace_path = project_dir / "workspace.json";
    std::ifstream input(workspace_path, std::ios::binary);
    if (input.is_open()) {
        try {
            nlohmann::json data;
            input >> data;
            if (data.is_object()) {
                out.cwd = data.value("cwd", std::string{});
                out.name = data.value("name", std::string{});
            }
        } catch (const std::exception& error) {
            LOG_WARN("[remote-control] ignored invalid workspace metadata at " +
                     path_to_utf8(workspace_path) + ": " + error.what());
        }
    }
    if (out.name.empty() && !out.cwd.empty()) {
        out.name = desktop::default_workspace_name(out.cwd);
    }
    return out;
}

RcSessionCatalogEntry from_meta(const SessionMeta& meta,
                                const WorkspaceSnapshot& workspace,
                                const std::string& storage_dir,
                                bool no_workspace) {
    RcSessionCatalogEntry entry;
    entry.id = meta.id;
    entry.title = meta.title;
    entry.summary = meta.summary;
    entry.cwd = meta.cwd.empty() ? workspace.cwd : meta.cwd;
    entry.workspace_hash = no_workspace ? std::string{} : workspace.hash;
    entry.workspace_name = no_workspace
                               ? std::string{}
                               : (workspace.name.empty()
                                      ? desktop::default_workspace_name(entry.cwd)
                                      : workspace.name);
    entry.updated_at = meta.updated_at.empty() ? meta.created_at : meta.updated_at;
    entry.storage_dir = storage_dir;
    entry.no_workspace = no_workspace;
    return entry;
}

RcSessionCatalogEntry from_active(const SessionInfo& info) {
    RcSessionCatalogEntry entry;
    entry.id = info.id;
    entry.title = info.title;
    entry.summary = info.summary;
    entry.cwd = info.cwd;
    entry.workspace_hash = info.no_workspace
                               ? std::string{}
                               : (info.workspace_hash.empty()
                                      ? SessionStorage::compute_project_hash(info.cwd)
                                      : info.workspace_hash);
    entry.workspace_name = info.no_workspace
                               ? std::string{}
                               : desktop::default_workspace_name(info.cwd);
    entry.updated_at = info.updated_at.empty() ? info.created_at : info.updated_at;
    entry.storage_dir = SessionStorage::get_project_dir(info.cwd);
    entry.no_workspace = info.no_workspace;
    entry.active = true;
    return entry;
}

bool ordinary_session(const SessionMeta& meta) {
    return !meta.id.empty() && !meta.archived && meta.parent_session_id.empty();
}

void merge_entry(std::unordered_map<std::string, RcSessionCatalogEntry>& by_key,
                 RcSessionCatalogEntry entry) {
    if (entry.id.empty()) return;
    const std::string key = rc_session_entry_key(entry);
    auto it = by_key.find(key);
    if (it == by_key.end()) {
        by_key.emplace(key, std::move(entry));
        return;
    }
    auto& current = it->second;
    if (!entry.title.empty()) current.title = std::move(entry.title);
    if (!entry.summary.empty()) current.summary = std::move(entry.summary);
    if (!entry.cwd.empty()) current.cwd = std::move(entry.cwd);
    if (!entry.workspace_hash.empty()) current.workspace_hash = std::move(entry.workspace_hash);
    if (!entry.workspace_name.empty()) current.workspace_name = std::move(entry.workspace_name);
    if (entry.updated_at > current.updated_at) current.updated_at = std::move(entry.updated_at);
    if (!entry.storage_dir.empty()) current.storage_dir = std::move(entry.storage_dir);
    current.no_workspace = entry.no_workspace;
    current.active = current.active || entry.active;
}

} // namespace

RcSessionCatalog::RcSessionCatalog(RcSessionCatalogDeps deps)
    : deps_(std::move(deps)) {}

std::vector<RcSessionCatalogEntry> RcSessionCatalog::list_all() const {
    std::unordered_map<std::string, RcSessionCatalogEntry> by_key;
    std::error_code ec;
    const fs::path projects = path_from_utf8(deps_.projects_dir);
    if (fs::is_directory(projects, ec)) {
        for (fs::directory_iterator it(projects, ec), end;
             !ec && it != end;
             it.increment(ec)) {
            std::error_code item_error;
            if (!it->is_directory(item_error) || item_error) continue;
            const std::string project_dir = path_to_utf8(it->path());
            const auto workspace = workspace_snapshot(it->path());
            for (const auto& meta : SessionStorage::list_sessions(project_dir)) {
                if (!ordinary_session(meta) || meta.no_workspace) continue;
                merge_entry(by_key, from_meta(meta, workspace, project_dir, false));
            }
        }
    }
    if (ec) {
        LOG_WARN("[remote-control] project session catalog scan failed: " + ec.message());
    }

    std::unordered_set<std::string> no_workspace_dirs;
    for (const auto& cwd : list_no_workspace_session_cwds(
             deps_.no_workspace_cache_root)) {
        const std::string project_dir = SessionStorage::get_project_dir(cwd);
        if (!no_workspace_dirs.insert(project_dir).second) continue;
        WorkspaceSnapshot workspace;
        workspace.cwd = cwd;
        for (const auto& meta : SessionStorage::list_sessions(project_dir)) {
            if (!ordinary_session(meta) || !meta.no_workspace) continue;
            merge_entry(by_key, from_meta(meta, workspace, project_dir, true));
        }
    }

    if (deps_.list_active_sessions) {
        for (const auto& info : deps_.list_active_sessions()) {
            if (info.id.empty() || !info.parent_session_id.empty()) continue;
            auto active = from_active(info);
            const auto meta = SessionStorage::read_meta(
                SessionStorage::meta_path(active.storage_dir, active.id));
            if (!meta.id.empty() && (meta.archived || !meta.parent_session_id.empty())) {
                continue;
            }
            merge_entry(by_key, std::move(active));
        }
    }

    std::vector<RcSessionCatalogEntry> out;
    out.reserve(by_key.size());
    for (auto& [_, entry] : by_key) out.push_back(std::move(entry));
    return sort_rc_sessions_newest_first(std::move(out));
}

std::vector<RcSessionCatalogEntry> RcSessionCatalog::search(
    const std::string& query,
    std::size_t limit) const {
    const auto entries = list_all();
    std::unordered_map<std::string, std::vector<const RcSessionCatalogEntry*>>
        entries_by_storage;
    for (const auto& entry : entries) {
        if (!entry.storage_dir.empty()) {
            entries_by_storage[entry.storage_dir].push_back(&entry);
        }
    }

    std::unordered_map<std::string, int> content_scores;
    for (const auto& [project_dir, project_entries] : entries_by_storage) {
        std::unordered_map<std::string, int> project_scores;
        try {
            if (deps_.search_content_scores) {
                project_scores = deps_.search_content_scores(project_dir, query);
            } else {
                SessionUserMessageIndex index(project_dir);
                std::string error;
                if (!index.ensure_project_indexed(&error)) {
                    LOG_WARN("[remote-control] user-message index unavailable for " +
                             project_dir + ": " + error);
                    continue;
                }
                const auto matches = index.search(query, 100, &error);
                if (!error.empty()) {
                    LOG_WARN("[remote-control] user-message search failed for " +
                             project_dir + ": " + error);
                    continue;
                }
                for (const auto& match : matches) {
                    project_scores[match.session_id] =
                        (std::max)(project_scores[match.session_id], match.score);
                }
            }
        } catch (const std::exception& error) {
            LOG_WARN("[remote-control] user-message search failed for " +
                     project_dir + ": " + error.what());
            continue;
        } catch (...) {
            LOG_WARN("[remote-control] user-message search failed for " + project_dir);
            continue;
        }
        for (const auto& [session_id, score] : project_scores) {
            for (const auto* entry : project_entries) {
                if (entry->id != session_id) continue;
                const std::string key = rc_session_entry_key(*entry);
                content_scores[key] = (std::max)(content_scores[key], score);
            }
        }
    }
    return search_rc_sessions(entries, query, content_scores, limit);
}

} // namespace acecode::rc
