#include "region_detector.hpp"

#include "network/proxy_resolver.hpp"
#include "utils/logger.hpp"
#include "region_cache.hpp"

#include <cpr/cpr.h>

#include <chrono>

namespace acecode::web_search {

namespace {

constexpr const char* kProbeUrl = "https://duckduckgo.com/";

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool is_reachable(long status) {
    // 2xx / 3xx 都算通(DDG 入口可能是 301 跳到 lite 页)
    return status >= 200 && status < 400;
}

} // namespace

static ProbeResult cpr_region_probe_owned(const std::string& url,
                                          const std::string& method,
                                          int timeout_ms,
                                          const network::ProxyOptions& opts) {
    ProbeResult r;
    cpr::Header headers = {
        {"User-Agent",
         "Mozilla/5.0 (compatible; ACECode-RegionProbe/1.0)"},
    };
    cpr::Response resp;
    if (method == "HEAD") {
        resp = cpr::Head(cpr::Url{url}, headers,
                         network::build_ssl_options(opts),
                         opts.proxies, opts.auth,
                         cpr::Timeout{timeout_ms});
    } else {
        // GET fallback — 不读 body 也能拿到 status
        resp = cpr::Get(cpr::Url{url}, headers,
                        network::build_ssl_options(opts),
                        opts.proxies, opts.auth,
                        cpr::Timeout{timeout_ms});
    }
    r.status_code = resp.status_code;
    if (resp.status_code == 0) r.error_message = resp.error.message;
    return r;
}

ProbeResult cpr_region_probe(const std::string& url,
                             const std::string& method, int timeout_ms) {
    return cpr_region_probe_owned(url, method, timeout_ms, network::proxy_options_for(url));
}

RegionDetector::RegionDetector(int timeout_ms, RegionProbeFn probe)
    : timeout_ms_(timeout_ms),
      probe_(std::move(probe)) {}

Region RegionDetector::detect_now(const std::atomic<bool>* abort) {
    if (abort && abort->load()) return Region::Unknown;

    const auto probe = probe_ ? probe_ : RegionProbeFn(cpr_region_probe);
    ProbeResult r = probe(kProbeUrl, "HEAD", timeout_ms_);
    if (abort && abort->load()) return Region::Unknown;

    // 405 = 服务器/代理拒绝 HEAD,回退到 GET
    if (r.status_code == 405) {
        r = probe(kProbeUrl, "GET", timeout_ms_);
        if (abort && abort->load()) return Region::Unknown;
    }

    Region region = is_reachable(r.status_code) ? Region::Global : Region::Cn;
    LOG_INFO(std::string("[web_search] region detected: ") + region_str(region) +
             " (probe status=" + std::to_string(r.status_code) +
             (r.error_message.empty() ? "" : ", err=" + r.error_message) + ")");

    WebSearchRegionCache cache;
    cache.region = region_str(region);
    cache.detected_at_ms = now_ms();
    write_web_search_region_cache(cache);
    return region;
}

std::function<Region()> RegionDetector::make_probe_task() const {
    auto probe = probe_;
    if (!probe) {
        auto opts = network::proxy_options_for(kProbeUrl);
        probe = [opts = std::move(opts)](const std::string& url,
                                        const std::string& method, int timeout) {
            return cpr_region_probe_owned(url, method, timeout, opts);
        };
    }
    return [probe = std::move(probe), timeout = timeout_ms_] {
        auto result = probe(kProbeUrl, "HEAD", timeout);
        if (result.status_code == 405) result = probe(kProbeUrl, "GET", timeout);
        return is_reachable(result.status_code) ? Region::Global : Region::Cn;
    };
}

void RegionDetector::publish_region(Region region) {
    WebSearchRegionCache cache;
    cache.region = region_str(region);
    cache.detected_at_ms = now_ms();
    write_web_search_region_cache(cache);
    LOG_INFO(std::string("[web_search] region detected: ") + region_str(region));
}

Region RegionDetector::get_or_detect(const std::atomic<bool>* abort) {
    auto cached = read_web_search_region_cache();
    if (cached.has_value()) {
        if (cached->region == "global") return Region::Global;
        if (cached->region == "cn") return Region::Cn;
    }
    return detect_now(abort);
}

void RegionDetector::invalidate() {
    clear_web_search_region_cache();
}

Region RegionDetector::cached_region() const {
    auto c = read_web_search_region_cache();
    if (!c.has_value()) return Region::Unknown;
    if (c->region == "global") return Region::Global;
    if (c->region == "cn") return Region::Cn;
    return Region::Unknown;
}

long long RegionDetector::cached_detected_at_ms() const {
    auto c = read_web_search_region_cache();
    return c.has_value() ? c->detected_at_ms : 0;
}

} // namespace acecode::web_search
