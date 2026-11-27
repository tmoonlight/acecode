#include "domestic_backend.hpp"

#include "html_utils.hpp"
#include "network/proxy_resolver.hpp"
#include "so360_parser.hpp"
#include "utils/url_encoding.hpp"

#include <cpr/cpr.h>
#include <curl/urlapi.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>

namespace acecode::web_search {
namespace {

using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
constexpr std::size_t kMaxBytes = 2 * 1024 * 1024;

HttpProbeResult request(const std::string& url, const std::string& json_body,
                        int timeout_ms, const std::atomic<bool>* abort) {
    HttpProbeResult out;
    auto proxy = network::proxy_options_for(url);
    cpr::Session session;
    session.SetUrl(cpr::Url{url});
    session.SetTimeout(cpr::Timeout{timeout_ms});
    session.SetProxies(proxy.proxies);
    session.SetProxyAuth(proxy.auth);
    session.SetSslOptions(network::build_ssl_options(proxy));
    cpr::Header headers{{"Accept-Language", "zh-CN,zh;q=0.9"},
                        {"User-Agent", "Mozilla/5.0 (compatible; ACECode-WebSearch/1.0)"}};
    if (json_body.empty()) {
        headers["Accept"] = "text/html";
    } else {
        headers["Content-Type"] = "application/json";
        headers["Accept"] = "application/json";
        session.SetBody(cpr::Body{json_body});
    }
    session.SetHeader(headers);
    session.SetProgressCallback(cpr::ProgressCallback{
        [abort](auto, auto, auto, auto, intptr_t) { return !abort || !abort->load(); }});
    session.SetWriteCallback(cpr::WriteCallback{[&out](std::string_view chunk, intptr_t) {
        if (chunk.size() > kMaxBytes - out.body.size()) return false;
        out.body.append(chunk);
        return true;
    }});
    const auto response = json_body.empty() ? session.Get() : session.Post();
    out.status_code = response.error.code == cpr::ErrorCode::OK ? response.status_code : 0;
    return out;
}

std::optional<SearchError> http_error(const HttpProbeResult& response, const char* source) {
    if (response.status_code == 200 && response.body.size() <= kMaxBytes) return {};
    if (response.status_code == 200) {
        return SearchError{SearchError::Kind::Parse, "response too large", source};
    }
    const bool limited = response.status_code == 402 || response.status_code == 429;
    return SearchError{limited ? SearchError::Kind::RateLimited : SearchError::Kind::Network,
                       response.status_code == 0 ? "request failed" :
                           "HTTP " + std::to_string(response.status_code), source};
}

bool usable_url(const std::string& url, std::string_view normalized, bool from_so360) {
    auto rest = normalized;
    if (rest.substr(0, 8) == "https://") rest.remove_prefix(8);
    else if (rest.substr(0, 7) == "http://") rest.remove_prefix(7);
    else return false;
    if (rest.empty() || rest.front() == '/' || rest.front() == '?') return false;
    if (std::any_of(url.begin(), url.end(), [](unsigned char c) {
            return c <= 0x20 || c == 0x7f || c == '\\';
        })) return false;
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
    if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), CURLU_DISALLOW_USER)
            != CURLUE_OK) return false;
    if (!from_so360) return true;
    const auto host = rest.substr(0, rest.find_first_of(":/?"));
    const auto path = rest.find_first_of("/?");
    return !((host == "so.com" || host == "www.so.com") && path != std::string_view::npos &&
             rest.substr(path, 5) == "/link");
}

using VisitHit = std::function<bool(SearchHit)>;

std::string field(const Json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::optional<SearchError> read_anysearch(const HttpProbeResult& response, const VisitHit& visit) {
    if (auto error = http_error(response, "anysearch")) return error;
    const auto root = Json::parse(response.body, nullptr, false);
    const auto data = root.find("data");
    const auto code = root.find("code");
    if (code == root.end() || *code != 0 || data == root.end() || !data->is_object()) {
        return SearchError{SearchError::Kind::Parse, "invalid search response", "anysearch"};
    }
    const auto results = data->find("results");
    if (results == data->end() || !results->is_array()) {
        return SearchError{SearchError::Kind::Parse, "missing results", "anysearch"};
    }
    for (const auto& item : *results) {
        SearchHit hit;
        hit.title = field(item, "title");
        hit.url = field(item, "url");
        hit.snippet = field(item, "snippet");
        hit.backend_name = "anysearch";
        if (!visit(std::move(hit))) break;
    }
    return {};
}

std::optional<SearchError> read_so360(const HttpProbeResult& response, const VisitHit& visit) {
    if (auto error = http_error(response, "so360")) return error;
    bool saw_result = false;
    visit_so360_results(response.body, [&](SearchHit hit) {
        saw_result = true;
        return visit(std::move(hit));
    });
    if (!saw_result) {
        if (response.body.find("wappass.so.com") != std::string::npos ||
            response.body.find("360搜索_访问验证") != std::string::npos) {
            return SearchError{SearchError::Kind::RateLimited, "verification required", "so360"};
        }
        return SearchError{SearchError::Kind::Parse, "no organic results found", "so360"};
    }
    return {};
}

} // namespace

DomesticBackend::DomesticBackend(int timeout_ms, DomesticFetchFn fetch)
    : timeout_ms_(timeout_ms), fetch_(fetch ? std::move(fetch) : request) {}

std::variant<SearchResponse, SearchError>
DomesticBackend::search(std::string_view query, int limit, const std::atomic<bool>* abort) {
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeout_ms_);
    const auto cancelled = [abort] { return abort && abort->load(); };
    const SearchError aborted{SearchError::Kind::Network, "aborted", name()};
    if (cancelled()) return aborted;

    limit = std::clamp(limit, 1, 10);
    SearchResponse out;
    out.backend_name = name();
    std::unordered_set<std::string> seen;
    const auto visit = [&](SearchHit hit) {
        hit.title = truncate_with_ellipsis(collapse_whitespace(hit.title), 512);
        if (hit.url.size() <= 4096 && !hit.title.empty()) {
            auto normalized = normalize_url_for_dedup(hit.url);
            if (usable_url(hit.url, normalized, hit.backend_name == "so360") &&
                seen.insert(std::move(normalized)).second) {
                hit.snippet = truncate_with_ellipsis(collapse_whitespace(hit.snippet), 2000);
                out.hits.push_back(std::move(hit));
            }
        }
        return !cancelled() && out.hits.size() < static_cast<std::size_t>(limit);
    };
    auto record = [&](const std::optional<SearchError>& error) {
        if (error) out.warnings.push_back(error->backend_name + ": " + error->message);
    };

    const Json body{{"query", query}, {"zone", "cn"}, {"language", "zh-CN"}, {"max_results", limit}};
    const auto primary = fetch_("https://api.anysearch.com/v1/search", body.dump(),
                                (std::max)(1, timeout_ms_ / 2), abort);
    if (cancelled()) return aborted;
    record(read_anysearch(primary, visit));
    if (cancelled()) return aborted;
    if (out.hits.size() < static_cast<std::size_t>(limit)) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now()).count();
        if (remaining > 0) {
            const auto url = "https://www.so.com/s?q=" + utils::percent_encode_query_component(query);
            const auto secondary = fetch_(url, {}, static_cast<int>(remaining), abort);
            if (cancelled()) return aborted;
            record(read_so360(secondary, visit));
        } else {
            out.warnings.emplace_back("so360: search deadline reached");
        }
    }
    if (cancelled()) return aborted;
    if (out.hits.empty()) {
        std::string message = "no usable domestic web results";
        for (const auto& warning : out.warnings) message += "; " + warning;
        return SearchError{SearchError::Kind::Network, std::move(message), name()};
    }
    out.duration_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start).count());
    return out;
}

} // namespace acecode::web_search
