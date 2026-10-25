#include "global_session_catalog_scan.hpp"

#include "workspace/workspace_registry.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <exception>
#include <unordered_map>

namespace fs = std::filesystem;
namespace acecode {
namespace {

using detail::ProjectScanResult;
using detail::make_error;
using detail::scan_project;

// The workspace a project directory describes, read once per project.
struct ProjectWorkspace {
    std::string project_dir;
    std::string workspace_hash;
    bool known = false;
    std::string cwd;
    std::string name;
    bool visible = false;
};

ProjectWorkspace project_workspace(const fs::path& project_path) {
    ProjectWorkspace result;
    result.project_dir = path_to_utf8(project_path);
    result.workspace_hash = path_to_utf8(project_path.filename());
    auto workspace = desktop::load_workspace_metadata(
        path_to_utf8(project_path.parent_path()), result.workspace_hash);
    if (workspace &&
        desktop::workspace_hash_matches_cwd(result.workspace_hash, workspace->cwd)) {
        result.known = true;
        result.cwd = workspace->cwd;
        result.name = workspace->name;
        result.visible = workspace->desktop_visible;
    }
    return result;
}

GlobalSessionCatalogEntry project_entry(const ProjectWorkspace& workspace,
                                        SessionMeta meta) {
    GlobalSessionCatalogEntry entry;
    entry.project_dir = workspace.project_dir;
    entry.workspace_hash = meta.no_workspace ? std::string{} : workspace.workspace_hash;
    entry.meta = std::move(meta);
    if (!entry.meta.no_workspace) {
        entry.workspace_cwd = workspace.known && !workspace.cwd.empty()
            ? workspace.cwd
            : entry.meta.cwd;
        entry.workspace_name = workspace.known && !workspace.name.empty()
            ? workspace.name
            : desktop::default_workspace_name(entry.workspace_cwd);
        entry.workspace_visible = workspace.known && workspace.visible;
    }
    return entry;
}

} // namespace

namespace detail {

ProjectScanResult scan_project(const fs::path& project_path,
                               const GlobalSessionCatalogOptions& options) {
    ProjectScanResult result;
    if (options.should_cancel && options.should_cancel()) {
        result.cancelled = true;
        return result;
    }
    const auto workspace = project_workspace(project_path);
    const std::string& project_dir = workspace.project_dir;
    const std::string& workspace_hash = workspace.workspace_hash;

    std::vector<SessionMeta> metas;
    try {
        metas = SessionStorage::list_session_metadata(
            project_dir, options.should_cancel);
        if (options.should_cancel && options.should_cancel()) {
            result.cancelled = true;
            return result;
        }
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
        if ((!options.include_archived && meta.archived) ||
            (!options.include_subagents && !meta.parent_session_id.empty())) {
            continue;
        }

        auto entry = project_entry(workspace, std::move(meta));
        auto match = matches_by_id.find(entry.meta.id);
        if (match != matches_by_id.end()) {
            entry.content_match = std::move(match->second);
        }
        result.entries.push_back(std::move(entry));
    }
    return result;
}

} // namespace detail

std::vector<GlobalSessionCatalogEntry> scan_global_session_project(
    const std::string& project_dir,
    const std::function<bool()>& should_cancel) {
    GlobalSessionCatalogOptions options;
    options.should_cancel = should_cancel;
    auto result = scan_project(path_from_utf8(project_dir), options);
    if (result.cancelled) return {};
    return std::move(result.entries);
}

GlobalSessionProjectReader::GlobalSessionProjectReader(const std::string& project_dir) {
    const auto workspace = project_workspace(path_from_utf8(project_dir));
    project_dir_ = workspace.project_dir;
    workspace_hash_ = workspace.workspace_hash;
    workspace_known_ = workspace.known;
    workspace_cwd_ = workspace.cwd;
    workspace_name_ = workspace.name;
    workspace_visible_ = workspace.visible;
}

std::vector<SessionStorage::MetaFileStamp> GlobalSessionProjectReader::list(
    const std::function<bool()>& should_cancel) const {
    return SessionStorage::list_meta_files(project_dir_, should_cancel);
}

std::optional<GlobalSessionCatalogEntry> GlobalSessionProjectReader::read(
    const std::string& meta_path) const {
    SessionMeta meta;
    try {
        meta = SessionStorage::read_meta(meta_path);
    } catch (...) {
        return std::nullopt;
    }
    if (meta.id.empty() || meta.archived || !meta.parent_session_id.empty()) return std::nullopt;
    ProjectWorkspace workspace;
    workspace.project_dir = project_dir_;
    workspace.workspace_hash = workspace_hash_;
    workspace.known = workspace_known_;
    workspace.cwd = workspace_cwd_;
    workspace.name = workspace_name_;
    workspace.visible = workspace_visible_;
    return project_entry(workspace, std::move(meta));
}

} // namespace acecode
