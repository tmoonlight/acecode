#pragma once

#include "rc_session_navigation.hpp"

#include "session/session_client.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace acecode::rc {

struct RcSessionCatalogDeps {
    std::string projects_dir;
    std::string no_workspace_cache_root;
    std::function<std::vector<SessionInfo>()> list_active_sessions;
    // Optional test seam. Keys are session ids inside project_dir; production
    // leaves this empty and uses SessionUserMessageIndex directly.
    std::function<std::unordered_map<std::string, int>(
        const std::string& project_dir,
        const std::string& query)> search_content_scores;
};

// Daemon-global persisted session catalog. Each invocation is an explicit
// snapshot; callers must run it off latency-sensitive HTTP/WS callbacks.
class RcSessionCatalog {
public:
    explicit RcSessionCatalog(RcSessionCatalogDeps deps);

    std::vector<RcSessionCatalogEntry> list_all() const;
    std::vector<RcSessionCatalogEntry> search(const std::string& query,
                                              std::size_t limit = 5) const;

private:
    RcSessionCatalogDeps deps_;
};

} // namespace acecode::rc
