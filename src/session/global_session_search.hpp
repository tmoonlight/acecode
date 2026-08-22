#pragma once

#include "global_session_catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

struct GlobalSessionSearchPage {
    std::vector<GlobalSessionCatalogEntry> entries;
    std::vector<GlobalSessionCatalogError> errors;
    GlobalSessionCatalogProgress progress;
    std::optional<std::string> next_cursor;
    bool cursor_stale = false;
};

struct GlobalSessionContentSearchPage {
    std::vector<GlobalSessionCatalogEntry> entries;
    std::vector<GlobalSessionCatalogError> errors;
    std::size_t scanned_projects = 0;
    std::size_t total_projects = 0;
    bool complete = false;
    bool cancelled = false;
};

// Process-owned search coordinator used by the daemon HTTP surface. Metadata
// pages are served from the incremental catalog. Content searches advance only
// a small, time-bounded project batch per call, so a UI can poll and cancel.
class GlobalSessionSearchService {
public:
    using ActiveSessionsProvider = GlobalSessionCatalogIndex::ActiveSessionsProvider;

    explicit GlobalSessionSearchService(
        std::string projects_dir,
        ActiveSessionsProvider active_sessions_provider = {});
    ~GlobalSessionSearchService();

    GlobalSessionSearchService(const GlobalSessionSearchService&) = delete;
    GlobalSessionSearchService& operator=(const GlobalSessionSearchService&) = delete;

    void start();
    void stop();

    GlobalSessionSearchPage search_sessions(
        const std::string& request_id,
        const std::string& query,
        std::size_t limit,
        const std::string& cursor = {});

    GlobalSessionContentSearchPage search_user_messages_batch(
        const std::string& request_id,
        const std::string& query,
        std::size_t limit);

    // Idempotent: returns true only for the first cancellation of an id.
    bool cancel(const std::string& request_id);

    void invalidate_project(const std::string& workspace_hash);
    void request_discovery();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acecode
