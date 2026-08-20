#pragma once

#include "session_client.hpp"
#include "session_storage.hpp"
#include "session_user_message_search.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

// One globally discoverable top-level session. Project/workspace data is
// descriptive metadata only: workspace visibility never decides whether the
// entry is present.
struct GlobalSessionCatalogEntry {
    std::string project_dir;
    std::string workspace_hash;
    std::string workspace_name;
    std::string workspace_cwd;
    bool workspace_visible = false;

    SessionMeta meta;
    std::optional<SessionInfo> active;
    std::optional<SessionUserMessageSearchResult> content_match;
};

struct GlobalSessionCatalogError {
    std::string project_dir;
    std::string workspace_hash;
    std::string stage;
    std::string message;
};

struct GlobalSessionCatalogOptions {
    // Absent or empty means metadata-only discovery. A non-empty query reuses
    // each project's existing user-message index and annotates matching rows.
    std::optional<std::string> content_query;
    int content_limit_per_project = 100;

    // Project scans use a bounded worker pool. Zero selects the default cap.
    std::size_t max_workers = 0;
};

struct GlobalSessionCatalog {
    std::vector<GlobalSessionCatalogEntry> entries;
    std::vector<GlobalSessionCatalogError> errors;
};

// Stable identity used for disk/live merging. no-workspace session ids are
// globally scoped; regular sessions are scoped by their persisted project hash.
std::string global_session_catalog_identity(bool no_workspace,
                                            const std::string& workspace_hash,
                                            const std::string& session_id);

// Enumerate every direct project directory under projects_dir, merge active
// sessions, and optionally annotate visible-user-message content matches.
GlobalSessionCatalog build_global_session_catalog(
    const std::string& projects_dir,
    const std::vector<SessionInfo>& active_sessions = {},
    const GlobalSessionCatalogOptions& options = {});

} // namespace acecode
