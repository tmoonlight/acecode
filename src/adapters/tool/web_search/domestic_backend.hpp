#pragma once

#include "duckduckgo_backend.hpp"

namespace acecode::web_search {

// Empty JSON body selects GET. The transport borrows the cancellation flag
// only for the synchronous request.
using DomesticFetchFn = std::function<HttpProbeResult(
    const std::string& url, const std::string& json_body, int timeout_ms,
    const std::atomic<bool>* abort)>;

class DomesticBackend final : public WebSearchBackend {
public:
    explicit DomesticBackend(int timeout_ms, DomesticFetchFn fetch = {});
    std::string name() const override { return "domestic"; }
    bool requires_api_key() const override { return false; }
    std::variant<SearchResponse, SearchError> search(
        std::string_view query, int limit, const std::atomic<bool>* abort) override;

private:
    int timeout_ms_;
    DomesticFetchFn fetch_;
};

} // namespace acecode::web_search
