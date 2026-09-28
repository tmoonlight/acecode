#pragma once

#include "backend.hpp"
#include "duckduckgo_backend.hpp" // HttpFetchFn / HttpProbeResult

#include <atomic>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace acecode::web_search {

class RssSearchBackend : public WebSearchBackend {
public:
    RssSearchBackend(std::string base_url,
                     int timeout_ms,
                     HttpFetchFn fetch = nullptr);

    std::string name() const override { return "rss"; }
    bool requires_api_key() const override { return false; }

    std::variant<SearchResponse, SearchError> search(
        std::string_view query,
        int limit,
        const std::atomic<bool>* abort) override;

private:
    std::string base_url_;
    int timeout_ms_;
    HttpFetchFn fetch_;
};

HttpProbeResult rss_http_get(const std::string& url, int timeout_ms);

std::variant<std::vector<SearchHit>, SearchError>
parse_rss_search_json(std::string_view body, int limit);

} // namespace acecode::web_search
