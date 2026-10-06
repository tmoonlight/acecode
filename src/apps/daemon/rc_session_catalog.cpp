#include "rc_session_catalog.hpp"

#include "session/global_session_catalog.hpp"
#include "utils/logger.hpp"
#include "workspace/workspace_registry.hpp"

namespace acecode::daemon {

std::vector<rc::RcSessionTarget> build_rc_session_catalog(const std::string& projects_dir,
                                                          SessionClient& client,
                                                          const std::optional<std::string>& query) {
    GlobalSessionCatalogOptions options;
    options.content_query = query;
    options.content_limit_per_project = 100;
    const auto catalog = build_global_session_catalog(projects_dir, client.list_sessions(), options);

    std::vector<rc::RcSessionTarget> out;
    out.reserve(catalog.entries.size());
    for (const auto& entry : catalog.entries) {
        const auto* active = entry.active ? &*entry.active : nullptr;
        rc::RcSessionTarget target;
        target.session_id = entry.meta.id;
        target.workspace_hash = entry.workspace_hash;
        target.cwd = active && !active->cwd.empty() ? active->cwd : entry.meta.cwd;
        target.title = active && !active->title.empty() ? active->title : entry.meta.title;
        target.summary = active && !active->summary.empty() ? active->summary : entry.meta.summary;
        target.workspace_label = entry.meta.no_workspace
            ? std::string{}
            : (!entry.workspace_name.empty() ? entry.workspace_name : desktop::default_workspace_name(target.cwd));
        target.updated_at = active && !active->updated_at.empty() ? active->updated_at : entry.meta.updated_at;
        target.no_workspace = entry.meta.no_workspace;
        target.active = active != nullptr;
        target.content_match_score = entry.content_match ? entry.content_match->score : 0;
        out.push_back(std::move(target));
    }
    for (const auto& error : catalog.errors) {
        LOG_WARN("[remote-control] global session catalog " + error.stage + " failed for " + error.project_dir +
                 ": " + error.message);
    }
    rc::sort_rc_session_targets(out, query.has_value());
    return out;
}

} // namespace acecode::daemon
