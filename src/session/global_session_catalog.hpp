#pragma once

#include "session_client.hpp"
#include "session_storage.hpp"
#include "session_user_message_search.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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

    // Optional cooperative cancellation. Metadata enumeration observes this
    // at file boundaries and the worker pool stops claiming new projects.
    std::function<bool()> should_cancel;
};

struct GlobalSessionCatalog {
    std::vector<GlobalSessionCatalogEntry> entries;
    std::vector<GlobalSessionCatalogError> errors;
};

struct GlobalSessionCatalogProgress {
    std::size_t scanned_projects = 0;
    std::size_t total_projects = 0;
    std::uint64_t generation = 0;
    bool complete = false;
    bool paused = false;
};

struct GlobalSessionCatalogIndexSnapshot {
    GlobalSessionCatalog catalog;
    GlobalSessionCatalogProgress progress;
    // Stable, sorted project paths used by incremental content-search jobs.
    std::vector<std::string> project_dirs;
};

// Bounded metadata-search view. The index ranks against immutable shard
// references and copies only the requested leading rows, so polling does not
// rebuild/copy the complete catalog while prewarming is still advancing.
struct GlobalSessionCatalogSelection {
    std::vector<GlobalSessionCatalogEntry> entries;
    std::vector<GlobalSessionCatalogError> errors;
    GlobalSessionCatalogProgress progress;
    bool truncated = false;
};

struct GlobalSessionCatalogProjectSnapshot {
    GlobalSessionCatalogProgress progress;
    std::uint64_t project_generation = 0;
    bool paths_changed = true;
    std::vector<std::string> project_dirs;
};

// Process-owned incremental catalog for interactive Web search. It keeps
// immutable per-project shards, prewarms them on one background worker, and
// pauses cooperatively when the last attached UI request is cancelled.
class GlobalSessionCatalogIndex {
public:
    using ActiveSessionsProvider = std::function<std::vector<SessionInfo>()>;

    explicit GlobalSessionCatalogIndex(
        std::string projects_dir,
        ActiveSessionsProvider active_sessions_provider = {});
    ~GlobalSessionCatalogIndex();

    GlobalSessionCatalogIndex(const GlobalSessionCatalogIndex&) = delete;
    GlobalSessionCatalogIndex& operator=(const GlobalSessionCatalogIndex&) = delete;

    void start();
    void stop();

    // Returns false when request_id was already cancelled; a cancelled id can
    // never be resurrected by a late/in-flight HTTP request.
    bool attach_request(const std::string& request_id);
    // Idempotent. Cancelling the last attached request pauses the worker at the
    // next file/project boundary while preserving committed shards.
    bool cancel_request(const std::string& request_id);
    bool is_request_cancelled(const std::string& request_id) const;

    void invalidate_project(const std::string& workspace_hash);
    void request_discovery();

    GlobalSessionCatalogIndexSnapshot snapshot() const;
    GlobalSessionCatalogSelection select_entries(
        std::size_t max_entries,
        const std::function<int(const GlobalSessionCatalogEntry&)>& score) const;
    GlobalSessionCatalogProjectSnapshot project_snapshot(
        std::optional<std::uint64_t> known_generation = std::nullopt) const;
    std::optional<GlobalSessionCatalogEntry> find_project_session(
        const std::string& project_dir,
        const std::string& session_id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
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
