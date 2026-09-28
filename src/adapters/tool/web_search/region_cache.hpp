#pragma once

#include <optional>
#include <string>

namespace acecode {

// 联网搜索 region 缓存(参见 add-web-search-tool 的 RegionDetector)。
// state.json 中的存储格式:
//   { "web_search": { "region_detected": "global"|"cn",
//                       "region_detected_at_ms": <epoch ms> } }
struct WebSearchRegionCache {
    std::string region;          // "global" / "cn"
    long long detected_at_ms = 0;
};

// 读取缓存的 region。文件不存在 / 字段缺失 / region 不是 global|cn → nullopt。
// detected_at_ms 即使为 0 / 缺失也接受(只表示来源未知,region 仍可信)。
std::optional<WebSearchRegionCache> read_web_search_region_cache();

// 写缓存的 region。原子写,保留其他 key。region 不在 {global, cn} 时静默不写。
void write_web_search_region_cache(const WebSearchRegionCache& cache);

// 删除 web_search 缓存(/websearch refresh 用)。其它 key 保留。
void clear_web_search_region_cache();

} // namespace acecode
