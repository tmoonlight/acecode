#include "global_session_catalog.hpp"

#include "../desktop/workspace_registry.hpp"
#include "../utils/cwd_hash.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <filesystem>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace acecode {

namespace {

constexpr std::size_t kDefaultMaxWorkers = 8;

struct ProjectScanResult {
    std::vector<GlobalSessionCatalogEntry> entries;
    std::vector<std::string> archived_identities;
    std::vector<GlobalSessionCatalogError> errors;
};

std::string effective_updated_at(const GlobalSessionCatalogEntry& entry) {
    if (entry.active && !entry.active->updated_at.empty()) {
        return entry.active->updated_at;
    }
    return entry.meta.updated_at;
}

int content_score(const GlobalSessionCatalogEntry& entry) {
    return entry.content_match ? entry.content_match->score : 0;
}

SessionMeta synthesize_meta(const SessionInfo& active) {
    SessionMeta meta;
    meta.id = active.id;
    meta.cwd = active.cwd;
    meta.created_at = active.created_at;
    meta.updated_at = active.updated_at;
    meta.summary = active.summary;
    meta.provider = active.provider;
    meta.model = active.model;
    meta.model_preset = active.model_name;
    meta.title = active.title;
    meta.title_source = active.title_source;
    meta.message_count = active.message_count;
    meta.turn_count = active.turn_count;
    meta.permission_mode = active.permission_mode;
    meta.last_token_usage = active.last_token_usage;
    meta.session_token_usage = active.session_token_usage;
    meta.parent_session_id = active.parent_session_id;
    meta.expert_id = active.expert_id;
    meta.expert_member_id = active.expert_member_id;
    meta.no_workspace = active.no_workspace;
    meta.worktree.worktree_path = active.worktree_path;
    meta.worktree.worktree_name = active.worktree_name;
    meta.worktree.worktree_branch = active.worktree_branch;
    return meta;
}

GlobalSessionCatalogError make_error(const std::string& project_dir,
                                     const std::string& workspace_hash,
                                     const std::string& stage,
                                     const std::string& message) {
    return GlobalSessionCatalogError{project_dir, workspace_hash, stage, message};
}

ProjectScanResult scan_project(const fs::path& project_path,
                               const GlobalSessionCatalogOptions& options) {
    ProjectScanResult result;
    const std::string project_dir = path_to_utf8(project_path);
    const std::string workspace_hash = path_to_utf8(project_path.filename());
    const std::string projects_dir = path_to_utf8(project_path.parent_path());
    auto workspace = desktop::load_workspace_metadata(projects_dir, workspace_hash);
    if (workspace &&
        !desktop::workspace_hash_matches_cwd(workspace_hash, workspace->cwd)) {
        workspace.reset();
    }

    std::vector<SessionMeta> metas;
    try {
        metas = SessionStorage::list_session_metadata(project_dir);
    } catch (const std::exception& e) {
        result.errors.push_back(make_error(
            project_dir, workspace_hash, "metadata", e.what()));
        return result;
    } catch (...) {
        result.errors.push_back(make_error(
            project_dir, workspace_hash, "metadata", "unknown project scan failure"));
        return result;
    }

    bool has_searchable_session = false;
    for (const auto& meta : metas) {
        if (!meta.archived && meta.parent_session_id.empty()) {
            has_searchable_session = true;
            break;
        }
    }

    std::unordered_map<std::string, SessionUserMessageSearchResult> matches_by_id;
    const std::string query = options.content_query.value_or(std::string{});
    if (has_searchable_session && !query.empty()) {
        SessionUserMessageIndex index(project_dir);
        std::string error;
        if (!index.ensure_project_indexed(&error)) {
            result.errors.push_back(make_error(
                project_dir,
                workspace_hash,
                "content-index",
                error.empty() ? "failed to refresh user-message index" : error));
        } else {
            const int per_project_limit = std::max(1, options.content_limit_per_project);
            auto matches = index.search(query, per_project_limit, &error);
            if (!error.empty()) {
                result.errors.push_back(make_error(
                    project_dir, workspace_hash, "content-search", error));
            } else {
                for (auto& match : matches) {
                    auto found = matches_by_id.find(match.session_id);
                    if (found == matches_by_id.end() || match.score > found->second.score) {
                        matches_by_id[match.session_id] = std::move(match);
                    }
                }
            }
        }
    }

    for (auto& meta : metas) {
        const std::string effective_hash = meta.no_workspace
            ? std::string{}
            : workspace_hash;
        const std::string identity = global_session_catalog_identity(
            meta.no_workspace, effective_hash, meta.id);
        if (meta.archived) {
            result.archived_identities.push_back(identity);
        }
        if (meta.archived || !meta.parent_session_id.empty()) continue;

        GlobalSessionCatalogEntry entry;
        entry.project_dir = project_dir;
        entry.workspace_hash = effective_hash;
        entry.meta = std::move(meta);
        if (!entry.meta.no_workspace) {
            entry.workspace_cwd = workspace && !workspace->cwd.empty()
                ? workspace->cwd
                : entry.meta.cwd;
            entry.workspace_name = workspace && !workspace->name.empty()
                ? workspace->name
                : desktop::default_workspace_name(entry.workspace_cwd);
            entry.workspace_visible = workspace && workspace->desktop_visible;
        }
        auto match = matches_by_id.find(entry.meta.id);
        if (match != matches_by_id.end()) {
            entry.content_match = std::move(match->second);
        }
        result.entries.push_back(std::move(entry));
    }
    return result;
}

std::size_t effective_worker_count(std::size_t requested, std::size_t project_count) {
    if (project_count == 0) return 0;
    std::size_t cap = requested == 0 ? kDefaultMaxWorkers : requested;
    cap = std::max<std::size_t>(1, cap);
    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware > 0) cap = std::min<std::size_t>(cap, hardware);
    return std::min(cap, project_count);
}

bool prefer_candidate(const GlobalSessionCatalogEntry& candidate,
                      const GlobalSessionCatalogEntry& current) {
    const std::string candidate_updated = effective_updated_at(candidate);
    const std::string current_updated = effective_updated_at(current);
    if (candidate_updated != current_updated) return candidate_updated > current_updated;
    return candidate.project_dir < current.project_dir;
}

void retain_best_content_match(GlobalSessionCatalogEntry& target,
                               const GlobalSessionCatalogEntry& other) {
    if (!other.content_match) return;
    if (!target.content_match ||
        other.content_match->score > target.content_match->score) {
        target.content_match = other.content_match;
    }
}

} // namespace

std::string global_session_catalog_identity(bool no_workspace,
    const std::string& workspace_hash,
    const std::string& session_id) {
    if (session_id.empty()) return {};
    if (no_workspace) return "no:" + session_id;
    return "ws:" + workspace_hash + ':' + session_id;
}

GlobalSessionCatalog build_global_session_catalog(
    const std::string& projects_dir,
    const std::vector<SessionInfo>& active_sessions,
    const GlobalSessionCatalogOptions& options) {
    GlobalSessionCatalog catalog;
    std::vector<fs::path> project_paths;
    const fs::path root = path_from_utf8(projects_dir);
    std::error_code ec;

    fs::directory_iterator it(root, ec);
    const fs::directory_iterator end;
    if (ec) {
        catalog.errors.push_back(make_error(
            projects_dir, {}, "projects", ec.message()));
    } else {
        for (; it != end; it.increment(ec)) {
            if (ec) {
                catalog.errors.push_back(make_error(
                    projects_dir, {}, "projects", ec.message()));
                break;
            }
            std::error_code item_ec;
            if (it->is_directory(item_ec) && !item_ec) {
                project_paths.push_back(it->path());
            } else if (item_ec) {
                catalog.errors.push_back(make_error(
                    path_to_utf8(it->path()), {}, "projects", item_ec.message()));
            }
        }
    }
    std::sort(project_paths.begin(), project_paths.end(), [](const fs::path& a, const fs::path& b) {
        return path_to_utf8(a) < path_to_utf8(b);
    });

    std::vector<ProjectScanResult> project_results(project_paths.size());
    const std::size_t worker_count = effective_worker_count(
        options.max_workers, project_paths.size());
    std::atomic<std::size_t> next_project{0};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&] {
            while (true) {
                const std::size_t index = next_project.fetch_add(1);
                if (index >= project_paths.size()) return;
                project_results[index] = scan_project(project_paths[index], options);
            }
        });
    }
    for (auto& worker : workers) worker.join();

    std::unordered_set<std::string> archived_identities;
    std::unordered_map<std::string, std::size_t> index_by_identity;
    for (auto& project : project_results) {
        for (auto& error : project.errors) {
            catalog.errors.push_back(std::move(error));
        }
        for (auto& identity : project.archived_identities) {
            archived_identities.insert(std::move(identity));
        }
        for (auto& entry : project.entries) {
            const std::string identity = global_session_catalog_identity(
                entry.meta.no_workspace, entry.workspace_hash, entry.meta.id);
            auto found = index_by_identity.find(identity);
            if (found == index_by_identity.end()) {
                index_by_identity.emplace(identity, catalog.entries.size());
                catalog.entries.push_back(std::move(entry));
                continue;
            }
            auto& current = catalog.entries[found->second];
            if (prefer_candidate(entry, current)) {
                retain_best_content_match(entry, current);
                current = std::move(entry);
            } else {
                retain_best_content_match(current, entry);
            }
        }
    }

    for (const auto& active : active_sessions) {
        if (active.id.empty() || !active.parent_session_id.empty()) continue;
        const std::string workspace_hash = active.no_workspace
            ? std::string{}
            : (!active.workspace_hash.empty()
                ? active.workspace_hash
                : compute_cwd_hash(active.cwd));
        const std::string identity = global_session_catalog_identity(
            active.no_workspace, workspace_hash, active.id);
        if (archived_identities.count(identity)) continue;

        auto found = index_by_identity.find(identity);
        if (found != index_by_identity.end()) {
            catalog.entries[found->second].active = active;
            continue;
        }

        GlobalSessionCatalogEntry entry;
        entry.workspace_hash = workspace_hash;
        entry.meta = synthesize_meta(active);
        entry.active = active;
        if (!active.no_workspace) {
            entry.project_dir = path_to_utf8(root / workspace_hash);
            const auto workspace = desktop::load_workspace_metadata(
                projects_dir, workspace_hash);
            entry.workspace_cwd = workspace && !workspace->cwd.empty()
                ? workspace->cwd
                : active.cwd;
            entry.workspace_name = workspace && !workspace->name.empty()
                ? workspace->name
                : desktop::default_workspace_name(entry.workspace_cwd);
            entry.workspace_visible = workspace && workspace->desktop_visible;
        } else if (!active.cwd.empty()) {
            const std::string hash = compute_cwd_hash(active.cwd);
            entry.project_dir = path_to_utf8(root / hash);
        }
        index_by_identity.emplace(identity, catalog.entries.size());
        catalog.entries.push_back(std::move(entry));
    }

    const bool prefer_content = options.content_query.has_value() &&
        !options.content_query->empty();
    std::sort(catalog.entries.begin(), catalog.entries.end(), [prefer_content](
        const GlobalSessionCatalogEntry& a,
        const GlobalSessionCatalogEntry& b) {
        if (prefer_content && content_score(a) != content_score(b)) {
            return content_score(a) > content_score(b);
        }
        const std::string au = effective_updated_at(a);
        const std::string bu = effective_updated_at(b);
        if (au != bu) return au > bu;
        if (a.meta.id != b.meta.id) return a.meta.id < b.meta.id;
        if (a.workspace_hash != b.workspace_hash) {
            return a.workspace_hash < b.workspace_hash;
        }
        return a.meta.cwd < b.meta.cwd;
    });
    std::sort(catalog.errors.begin(), catalog.errors.end(), [](
        const GlobalSessionCatalogError& a,
        const GlobalSessionCatalogError& b) {
        if (a.project_dir != b.project_dir) return a.project_dir < b.project_dir;
        if (a.stage != b.stage) return a.stage < b.stage;
        return a.message < b.message;
    });
    return catalog;
}

} // namespace acecode
