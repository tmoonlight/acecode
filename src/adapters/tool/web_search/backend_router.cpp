#include "backend_router.hpp"

#include "duckduckgo_backend.hpp"
#include "domestic_backend.hpp"
#include "html_utils.hpp"
#include "rss_search_backend.hpp"
#include "utils/logger.hpp"
#include "utils/abandonable_call.hpp"

#include <array>
#include <chrono>
#include <future>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace acecode::web_search {

namespace {

using SearchOutcome = std::variant<SearchResponse, SearchError>;

SearchOutcome search_cancellable(std::shared_ptr<WebSearchBackend> backend,
                                std::string query, int limit,
                                const std::atomic<bool>* abort) {
    const auto name = backend->name();
    auto result = run_cancellable<SearchOutcome>(
        [backend = std::move(backend), query = std::move(query), limit]
        (const std::atomic<bool>* cancellation) {
            return backend->search(query, limit, cancellation);
        }, abort);
    if (result) return std::move(*result);
    return SearchError{SearchError::Kind::Network, "aborted", name};
}

bool is_known_backend_name(const std::string& name) {
    return name == "parallel" || name == "rss" || name == "domestic" ||
           name == "duckduckgo" || name == "bing_cn" ||
           name == "bochaai" || name == "tavily";
}

std::array<const char*, 2> parallel_sources(Region region) {
    if (region == Region::Global) return {"rss", "duckduckgo"};
    return {"domestic", "rss"};
}

std::string bounded_warning(const std::string& backend,
                            const SearchError& error) {
    constexpr std::size_t kMaxWarningLength = 240;
    std::string warning = backend + ": " + error.message;
    if (warning.size() > kMaxWarningLength) {
        warning.resize(kMaxWarningLength - 3);
        warning += "...";
    }
    return warning;
}

} // namespace

BackendRouter::BackendRouter(const WebSearchConfig& cfg) : cfg_(cfg) {}

void BackendRouter::register_backend(std::unique_ptr<WebSearchBackend> b) {
    std::lock_guard<std::mutex> lk(mu_);
    if (b) {
        std::string name = b->name();
        backends_[std::move(name)] = std::move(b);
    }
}

std::string BackendRouter::compute_active_name(Region region) const {
    if (cfg_.backend == "auto") {
        return region == Region::Global ? "duckduckgo" : "domestic";
    }
    if (cfg_.backend == "bing_cn" ||
        cfg_.backend == "bochaai" || cfg_.backend == "tavily") {
        return "duckduckgo";
    }
    return cfg_.backend;
}

void BackendRouter::resolve_active(Region region) {
    std::lock_guard<std::mutex> lk(mu_);
    resolved_region_ = region;
    if (!manual_selection_) resolve_config_unlocked(region);
}

void BackendRouter::resolve_config_unlocked(Region region) {
    std::string desired = compute_active_name(region);
    if (desired == "parallel") {
        if (!backends_.empty()) {
            active_ = "parallel";
        } else {
            active_.clear();
            LOG_WARN("[web_search] no backends registered, web_search disabled");
        }
        return;
    }
    if (backends_.find(desired) == backends_.end()) {
        for (const auto* fallback : {"duckduckgo", "domestic", "rss"}) {
            if (backends_.count(fallback)) {
                active_ = fallback;
                return;
            }
        }
        active_.clear();
        LOG_WARN("[web_search] no usable backend registered");
        return;
    }
    active_ = std::move(desired);
}

bool BackendRouter::set_active(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    if (name == "bing_cn") {
        return false;
    }
    if (name == "parallel") {
        for (const char* backend_name : parallel_sources(resolved_region_)) {
            if (backends_.find(backend_name) != backends_.end()) {
                active_ = name;
                manual_selection_ = true;
                return true;
            }
        }
        return false;
    }
    if (backends_.find(name) == backends_.end()) {
        return false;
    }
    active_ = name;
    manual_selection_ = true;
    return true;
}

void BackendRouter::reset_to_config(Region region) {
    std::lock_guard<std::mutex> lk(mu_);
    resolved_region_ = region;
    manual_selection_ = false;
    resolve_config_unlocked(region);
}

std::string BackendRouter::active_name() const {
    std::lock_guard<std::mutex> lk(mu_);
    return active_;
}

std::shared_ptr<WebSearchBackend>
BackendRouter::find_unlocked(const std::string& name) {
    auto it = backends_.find(name);
    return (it == backends_.end()) ? nullptr : it->second;
}

std::variant<SearchResponse, SearchError>
BackendRouter::search_parallel(std::string_view query, int limit,
                               const std::atomic<bool>* abort,
                               const NotifyFn& notify) {
    using Outcome = std::variant<SearchResponse, SearchError>;
    const auto started = std::chrono::steady_clock::now();

    std::array<std::shared_ptr<WebSearchBackend>, 2> backends;
    std::array<const char*, 2> sources;
    {
        std::lock_guard<std::mutex> lk(mu_);
        sources = parallel_sources(resolved_region_);
        for (std::size_t i = 0; i < sources.size(); ++i) {
            backends[i] = find_unlocked(sources[i]);
        }
    }

    std::array<std::future<Outcome>, 2> futures;
    std::array<bool, 2> launched{};
    for (std::size_t i = 0; i < backends.size(); ++i) {
        if (!backends[i]) continue;
        auto backend = backends[i];
        const std::string backend_name = sources[i];
        launched[i] = true;
        futures[i] = std::async(std::launch::async,
            [backend = std::move(backend), backend_name, query = std::string(query),
             limit, abort]() -> Outcome {
                try {
                    return search_cancellable(backend, query, limit, abort);
                } catch (const std::exception& e) {
                    return SearchError{SearchError::Kind::Network,
                                       "search threw an exception: " +
                                           std::string(e.what()),
                                       backend_name};
                } catch (...) {
                    return SearchError{SearchError::Kind::Network,
                                       "search threw an unknown exception",
                                       backend_name};
                }
            });
    }

    std::array<Outcome, 2> outcomes = {
        SearchError{SearchError::Kind::Disabled, "backend is not registered", sources[0]},
        SearchError{SearchError::Kind::Disabled, "backend is not registered", sources[1]},
    };
    for (std::size_t i = 0; i < futures.size(); ++i) {
        if (launched[i]) outcomes[i] = futures[i].get();
    }

    if (abort && abort->load()) {
        return SearchError{SearchError::Kind::Network, "aborted", "parallel"};
    }

    SearchResponse combined;
    combined.backend_name = "parallel";
    std::array<const SearchResponse*, 2> successful{};
    std::size_t success_count = 0;
    for (std::size_t i = 0; i < outcomes.size(); ++i) {
        if (std::holds_alternative<SearchResponse>(outcomes[i])) {
            successful[i] = &std::get<SearchResponse>(outcomes[i]);
            const auto& warnings = successful[i]->warnings;
            combined.warnings.insert(combined.warnings.end(), warnings.begin(), warnings.end());
            ++success_count;
        } else {
            combined.warnings.push_back(bounded_warning(
                sources[i], std::get<SearchError>(outcomes[i])));
        }
    }

    const bool empty_domestic_search = sources[0] == std::string_view("domestic") &&
        !successful[0] && successful[1] && successful[1]->hits.empty();
    if (success_count == 0 || empty_domestic_search) {
        std::ostringstream message;
        message << (empty_domestic_search ? "no usable web search results" :
                                            "all web search backends failed");
        for (const auto& warning : combined.warnings) {
            message << "; " << warning;
        }
        return SearchError{SearchError::Kind::Network, message.str(), "parallel"};
    }

    const std::size_t result_limit = limit > 0
        ? static_cast<std::size_t>(limit) : 0;
    std::array<std::size_t, 2> positions{};
    std::unordered_set<std::string> seen_urls;
    while (combined.hits.size() < result_limit) {
        bool made_progress = false;
        for (std::size_t i = 0;
             i < successful.size() && combined.hits.size() < result_limit; ++i) {
            const auto* response = successful[i];
            if (!response) continue;
            while (positions[i] < response->hits.size()) {
                SearchHit hit = response->hits[positions[i]++];
                const std::string normalized = normalize_url_for_dedup(hit.url);
                if (!normalized.empty() && !seen_urls.insert(normalized).second) {
                    continue;
                }
                if (hit.backend_name.empty()) hit.backend_name = sources[i];
                combined.hits.push_back(std::move(hit));
                made_progress = true;
                break;
            }
        }
        if (!made_progress) break;
    }

    combined.duration_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
    if (!combined.warnings.empty() && notify) {
        std::ostringstream warning_message;
        warning_message << "Web search partial failure: ";
        for (std::size_t i = 0; i < combined.warnings.size(); ++i) {
            if (i) warning_message << "; ";
            warning_message << combined.warnings[i];
        }
        notify(warning_message.str());
    }
    return combined;
}

std::variant<SearchResponse, SearchError>
BackendRouter::search_with_fallback(std::string_view query, int limit,
                                     const std::atomic<bool>* abort,
                                     const NotifyFn& notify) {
    // snapshot 当前 active + 拿到 backend 指针;mutex 持有时间最短。
    std::string primary;
    std::shared_ptr<WebSearchBackend> primary_be;
    {
        std::lock_guard<std::mutex> lk(mu_);
        primary = active_;
        primary_be = find_unlocked(primary);
    }
    if (primary == "parallel") {
        return search_parallel(query, limit, abort, notify);
    }
    if (!primary_be) {
        return SearchError{SearchError::Kind::Disabled,
                           "no active web search backend",
                           ""};
    }

    auto first = search_cancellable(primary_be, std::string(query), limit, abort);
    if (abort && abort->load()) return first;
    const bool rss_primary = primary == "rss";
    std::string rss_fallback_reason;
    if (std::holds_alternative<SearchResponse>(first)) {
        const auto& response = std::get<SearchResponse>(first);
        if (!rss_primary || !response.hits.empty()) return first;
        rss_fallback_reason = "no matching RSS items";
    } else {
        const auto& err = std::get<SearchError>(first);
        if (abort && abort->load()) return first;
        if (!rss_primary) return first;
        if (rss_primary && err.kind != SearchError::Kind::Network &&
            err.kind != SearchError::Kind::RateLimited) {
            return first;
        }
        if (rss_primary) rss_fallback_reason = err.message;
    }

    // RSS always uses DuckDuckGo as its per-request fallback. Bing CN is
    // intentionally excluded from every automatic path.
    const std::string fallback_name = "duckduckgo";
    std::shared_ptr<WebSearchBackend> fallback_be;
    {
        std::lock_guard<std::mutex> lk(mu_);
        fallback_be = fallback_name.empty() ? nullptr : find_unlocked(fallback_name);
    }
    if (!fallback_be) {
        return first;
    }

    auto second = search_cancellable(fallback_be, std::string(query), limit, abort);
    if (abort && abort->load()) return second;
    if (std::holds_alternative<SearchResponse>(second)) {
        if (notify) {
            notify("RSS search had no usable result; used " + fallback_name +
                   " for this request (" + rss_fallback_reason + ")");
        }
        return second;
    }
    // RSS + DDG 都失败:返回第二次的错误(更近的事实),不 notify。
    return second;
}

nlohmann::json BackendRouter::status_snapshot(Region region) const {
    std::lock_guard<std::mutex> lk(mu_);
    nlohmann::json j;
    j["active_backend"] = active_;
    j["config_backend"] = cfg_.backend;
    j["region"] = region_str(region);
    j["enabled"] = cfg_.enabled;
    nlohmann::json registered = nlohmann::json::array();
    for (const auto& [name, _] : backends_) registered.push_back(name);
    j["registered"] = std::move(registered);
    return j;
}

std::vector<std::string> BackendRouter::registered_names_for_test() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(backends_.size());
    for (const auto& [name, _] : backends_) out.push_back(name);
    return out;
}

void register_default_backends(BackendRouter& router, const WebSearchConfig& cfg) {
    router.register_backend(std::make_unique<DomesticBackend>(cfg.timeout_ms));
    router.register_backend(
        std::make_unique<RssSearchBackend>(cfg.rss_base_url, cfg.timeout_ms));
    router.register_backend(
        std::make_unique<DuckDuckGoBackend>(cfg.timeout_ms));

    if (cfg.backend == "bing_cn") {
        LOG_WARN("[web_search] backend 'bing_cn' is disabled due to result quality; "
                 "using duckduckgo");
    } else if (cfg.backend == "bochaai" || cfg.backend == "tavily") {
        LOG_WARN("[web_search] backend '" + cfg.backend +
                 "' not implemented yet; using duckduckgo");
    }
    if (!is_known_backend_name(cfg.backend) && cfg.backend != "auto") {
        // load_config 应该已挡住,这里只是双保险。
        LOG_WARN("[web_search] unknown backend '" + cfg.backend +
                 "' in config; falling back to auto");
    }
}

} // namespace acecode::web_search
