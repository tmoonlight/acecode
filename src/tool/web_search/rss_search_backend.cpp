#include "rss_search_backend.hpp"

#include "html_utils.hpp"
#include "network/proxy_resolver.hpp"
#include "utils/http_url_validation.hpp"
#include "utils/url_encoding.hpp"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <string>
#include <utility>

namespace acecode::web_search {

namespace {

constexpr std::size_t kMaxResponseBytes = 2 * 1024 * 1024;
constexpr std::size_t kMaxTitleCodepoints = 512;
constexpr std::size_t kMaxSnippetCodepoints = 2000;
constexpr std::size_t kMaxSourceCodepoints = 256;
constexpr std::size_t kMaxPublishedAtCodepoints = 128;
constexpr const char* kUserAgent = "ACECode-WebSearch/1.0";

std::size_t utf8_codepoint_count(std::string_view text) {
    std::size_t count = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) ++count;
    }
    return count;
}

bool is_http_url(std::string_view url) {
    if (url.size() < 7 || url.size() > 4096) return false;
    for (unsigned char c : url) {
        if (c <= 0x20 || c == 0x7f) return false;
    }
    std::string prefix(url.substr(0, std::min<std::size_t>(8, url.size())));
    std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return prefix.rfind("http://", 0) == 0 || prefix.rfind("https://", 0) == 0;
}

std::string optional_string(const nlohmann::json& item,
                            const char* key,
                            std::size_t max_codepoints) {
    auto it = item.find(key);
    if (it == item.end() || !it->is_string()) return {};
    return truncate_with_ellipsis(it->get<std::string>(), max_codepoints);
}

SearchError parse_error(std::string message) {
    return SearchError{SearchError::Kind::Parse, std::move(message), "rss"};
}

} // namespace

HttpProbeResult rss_http_get(const std::string& url, int timeout_ms) {
    HttpProbeResult out;
    bool too_large = false;
    std::string body;
    body.reserve(64 * 1024);
    auto proxy_opts = network::proxy_options_for(url);
    cpr::Header headers = {
        {"User-Agent", kUserAgent},
        {"Accept", "application/json"},
    };
    cpr::Response response = cpr::Download(
        cpr::WriteCallback{[&](std::string_view chunk, intptr_t) {
            if (body.size() + chunk.size() > kMaxResponseBytes) {
                const std::size_t remaining = kMaxResponseBytes - body.size();
                body.append(chunk.data(), remaining);
                body.push_back('\0'); // bounded sentinel: size == cap + 1
                too_large = true;
                return false;
            }
            body.append(chunk.data(), chunk.size());
            return true;
        }},
        cpr::Url{url},
        headers,
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{timeout_ms});
    out.status_code = too_large ? 200 : response.status_code;
    out.body = std::move(body);
    if (response.status_code == 0 && !too_large) {
        out.error_message = response.error.message;
    }
    return out;
}

std::variant<std::vector<SearchHit>, SearchError>
parse_rss_search_json(std::string_view body, int limit) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(body);
    } catch (const std::exception& e) {
        return parse_error(std::string("invalid JSON response: ") + e.what());
    }
    if (!root.is_object()) return parse_error("response root must be an object");
    auto results_it = root.find("results");
    if (results_it == root.end() || !results_it->is_array()) {
        return parse_error("response must contain a results array");
    }

    const int bounded_limit = std::max(1, std::min(limit, 10));
    std::vector<SearchHit> hits;
    hits.reserve(static_cast<std::size_t>(bounded_limit));
    for (const auto& item : *results_it) {
        if (hits.size() >= static_cast<std::size_t>(bounded_limit)) break;
        if (!item.is_object()) continue;
        auto title_it = item.find("title");
        auto url_it = item.find("url");
        if (title_it == item.end() || !title_it->is_string() ||
            url_it == item.end() || !url_it->is_string()) {
            continue;
        }
        SearchHit hit;
        hit.title = truncate_with_ellipsis(
            collapse_whitespace(title_it->get<std::string>()), kMaxTitleCodepoints);
        hit.url = url_it->get<std::string>();
        if (hit.title.empty() || !is_http_url(hit.url)) continue;
        hit.snippet = collapse_whitespace(
            optional_string(item, "snippet", kMaxSnippetCodepoints));
        hit.source = collapse_whitespace(
            optional_string(item, "source", kMaxSourceCodepoints));
        hit.published_at = collapse_whitespace(optional_string(
            item, "published_at", kMaxPublishedAtCodepoints));
        hits.push_back(std::move(hit));
    }
    return hits;
}

RssSearchBackend::RssSearchBackend(std::string base_url,
                                   int timeout_ms,
                                   HttpFetchFn fetch)
    : base_url_(std::move(base_url)),
      timeout_ms_(timeout_ms),
      fetch_(fetch ? std::move(fetch) : rss_http_get) {
    while (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();
    if (base_url_.empty()) base_url_ = "https://ge.bigjuan.xyz/rss-search";
}

std::variant<SearchResponse, SearchError>
RssSearchBackend::search(std::string_view query,
                         int limit,
                         const std::atomic<bool>* abort) {
    if (abort && abort->load()) {
        return SearchError{SearchError::Kind::Network, "aborted", name()};
    }
    if (!utils::is_valid_http_base_url(base_url_)) {
        return SearchError{SearchError::Kind::Disabled,
                           "invalid RSS search base URL", name()};
    }
    if (utf8_codepoint_count(query) > 200) {
        return SearchError{SearchError::Kind::Disabled,
                           "web search query exceeds 200 characters", name()};
    }
    const int bounded_limit = std::max(1, std::min(limit, 10));
    const std::string url = base_url_ + "/v1/search?q=" +
        utils::percent_encode_query_component(query) +
        "&limit=" + std::to_string(bounded_limit);
    const auto started = std::chrono::steady_clock::now();
    HttpProbeResult response = fetch_(url, timeout_ms_);
    if (abort && abort->load()) {
        return SearchError{SearchError::Kind::Network, "aborted", name()};
    }
    if (response.status_code == 0) {
        return SearchError{SearchError::Kind::Network,
                           "RSS search service unreachable: " + response.error_message,
                           name()};
    }
    if (response.status_code == 429) {
        return SearchError{SearchError::Kind::RateLimited,
                           "RSS search service returned HTTP 429", name()};
    }
    if (response.status_code != 200) {
        return SearchError{SearchError::Kind::Network,
                           "RSS search service returned HTTP " +
                               std::to_string(response.status_code),
                           name()};
    }
    if (response.body.size() > kMaxResponseBytes) {
        return parse_error("response too large");
    }
    auto parsed = parse_rss_search_json(response.body, bounded_limit);
    if (std::holds_alternative<SearchError>(parsed)) {
        return std::get<SearchError>(std::move(parsed));
    }

    SearchResponse out;
    out.hits = std::get<std::vector<SearchHit>>(std::move(parsed));
    out.backend_name = name();
    out.duration_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
    return out;
}

} // namespace acecode::web_search
