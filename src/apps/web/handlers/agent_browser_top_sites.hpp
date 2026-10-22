#pragma once

// Agent Browser 新标签页「推荐」的数据来源:常访问网站。
//
// 对照 Codex 桌面版:它的新标签页读 Chromium 内核的 TopSites(按访问频率排出的
// 常访问网站),取前 4 个,允许用户移除单个推荐并撤销。ACECode 的 Agent Browser 是
// WebView2 / WKWebView,拿不到浏览历史,所以由前端在页面加载完成时上报访问,
// daemon 记在 <data_dir>/agent_browser_top_sites.json(Desktop 每次启动端口会变,
// 前端 localStorage 跨启动保不住)。
//
// 本文件只有纯逻辑(文档归一、记访问、排序、移除 / 恢复)与整份读写,路由在
// routes/routes_agent_browser.cpp。

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace acecode::web::agent_browser_top_sites {

// 存多少个网站 / 多少条已移除记录。超出时丢掉分数最低的网站、最早移除的记录。
inline constexpr std::size_t kMaxStoredSites = 200;
inline constexpr std::size_t kMaxBlockedUrls = 200;
// 新标签页一次最多展示几个推荐(Codex 同样是 4 个)。
inline constexpr std::size_t kDefaultDisplayLimit = 4;
inline constexpr std::size_t kMaxDisplayLimit = 12;
inline constexpr std::size_t kMaxUrlBytes = 2048;
inline constexpr std::size_t kMaxTitleBytes = 300;
// 同源图标由 Desktop 抓成 data URL(上限 256KB),推荐只要 32px 图标,过大的不存,
// 前端退回地球图标。
inline constexpr std::size_t kMaxFaviconDataUrlBytes = 64 * 1024;

struct Visit {
    std::string url;
    std::string title;
    std::string favicon;
    // true = 一次新的导航(计数 +1);false = 同一次访问后到的标题 / 图标补充,
    // 只更新已有记录,不新建。
    bool new_visit = true;
};

std::string file_path(const std::string& data_dir);

// 只接受 http / https,去掉 #fragment;不合法返回空串。
std::string normalize_url(const std::string& url);
// http(s) 地址或 data:image/ 且不超上限;不合法返回空串。
std::string normalize_favicon(const std::string& favicon);

// 把磁盘内容整理成合法文档:丢弃坏条目、合并重复 URL、套用上限。
nlohmann::json normalize_document(const nlohmann::json& raw);
nlohmann::json empty_document();

// 访问的推荐分数:访问次数 × 按最近访问时间分档的权重(4 天内 1.0、14 天内 0.7、
// 31 天内 0.5、90 天内 0.3、更早 0.1,取自 Firefox frecency 的时间分档)。
double site_score(const nlohmann::json& site, std::int64_t now_ms);

// 记一次访问;返回文档是否变化(需要落盘)。
bool record_visit(nlohmann::json& doc, const Visit& visit, std::int64_t now_ms);
// 移除 / 恢复一个推荐;返回文档是否变化。
bool dismiss(nlohmann::json& doc, const std::string& url);
bool restore(nlohmann::json& doc, const std::string& url);

// 前端展示用:去掉已移除的,按分数排序取前 limit 个,每项 {url, title, favicon}。
// 没有标题时用主机名。
nlohmann::json display_sites(const nlohmann::json& doc,
                             std::size_t limit,
                             std::int64_t now_ms);

// 读不到 / 损坏都返回空文档(损坏时 warning 非空)。
nlohmann::json load(const std::string& path, std::string* warning = nullptr);
bool save(const std::string& path, const nlohmann::json& doc, std::string* error = nullptr);

} // namespace acecode::web::agent_browser_top_sites
