#include "region_cache.hpp"

#include "utils/logger.hpp"
#include "utils/state_file.hpp"

#include <nlohmann/json.hpp>
#include <utility>

namespace acecode {

std::optional<WebSearchRegionCache> read_web_search_region_cache() {
    auto j = read_state_json();
    if (!j.contains("web_search") || !j["web_search"].is_object()) return std::nullopt;
    const auto& wsj = j["web_search"];
    if (!wsj.contains("region_detected") || !wsj["region_detected"].is_string()) {
        return std::nullopt;
    }
    std::string region = wsj["region_detected"].get<std::string>();
    if (region != "global" && region != "cn") return std::nullopt;
    WebSearchRegionCache c;
    c.region = std::move(region);
    if (wsj.contains("region_detected_at_ms") &&
        wsj["region_detected_at_ms"].is_number_integer()) {
        c.detected_at_ms = wsj["region_detected_at_ms"].get<long long>();
    }
    return c;
}

void write_web_search_region_cache(const WebSearchRegionCache& cache) {
    if (cache.region != "global" && cache.region != "cn") {
        LOG_WARN("[state_file] refusing to write web_search region '" +
                 cache.region + "' (must be global or cn)");
        return;
    }
    nlohmann::json wsj = nlohmann::json::object();
    wsj["region_detected"] = cache.region;
    wsj["region_detected_at_ms"] = cache.detected_at_ms;
    (void)update_state_json([wsj = std::move(wsj)](nlohmann::json& state) {
        state["web_search"] = wsj;
        return true;
    });
}

void clear_web_search_region_cache() {
    (void)update_state_json([](nlohmann::json& state) {
        // 字段不存在时不重写文件,保留原来的无操作语义。
        return state.erase("web_search") != 0;
    });
}

} // namespace acecode
