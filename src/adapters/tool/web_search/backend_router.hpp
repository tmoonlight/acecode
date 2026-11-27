#pragma once

// 默认并发查询 RSS 和按地区选择的网页来源。
// 线程安全(内部互斥锁 + shared backend ownership)。
//
// 详见 openspec/changes/integrate-rss-web-search/design.md。

#include "backend.hpp"
#include "region_detector.hpp"
#include "config/config.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <variant>

namespace acecode::web_search {

// notify 用于把部分失败或 RSS fallback 信息回传给上层(TUI / 工具流)。
// notify 可以为空(测试 / daemon 不关心可以传 nullptr)。
using NotifyFn = std::function<void(const std::string& yellow_message)>;

class BackendRouter {
public:
    // cfg 引用要在 router 生命周期内保持有效(通常 cfg 是进程级 AppConfig)。
    explicit BackendRouter(const WebSearchConfig& cfg);

    // 注册一个 backend(name 即 backend->name())。生产代码通常在构造后立即
    // 调 register_default_backends(...) 注入生产搜索源。
    void register_backend(std::unique_ptr<WebSearchBackend> b);

    // 发布地区更新。用户手动选择后，只有 reset 才恢复配置。
    void resolve_active(Region region);

    // 显式切 active backend(/websearch use)— 不持久化。parallel 是虚拟
    // backend;其他未知 / 未注册的 backend 名返回 false 并保持当前不变。
    bool set_active(const std::string& name);

    // 清除会话选择，重新应用配置与缓存地区。
    void reset_to_config(Region region);

    // 当前 active backend 名(线程安全)。无 active 时返回空字符串。
    std::string active_name() const;

    // 一次搜索。parallel 并发查询 RSS 和区域网页后端并轮询合并；显式 RSS
    // 可按请求回退 DDG,DDG 失败不再回退 Bing CN。
    std::variant<SearchResponse, SearchError> search_with_fallback(
        std::string_view query,
        int limit,
        const std::atomic<bool>* abort,
        const NotifyFn& notify);

    // /websearch 命令显示用的状态快照。
    nlohmann::json status_snapshot(Region region) const;

    // 测试 hook:列出已注册的 backend 名(按 name 排序)。
    std::vector<std::string> registered_names_for_test() const;

private:
    // 计算 cfg + region 推导出的 active backend 名(纯函数,线程安全外取锁前调)。
    // 不访问 backends_;只看 cfg + region + 静态映射。
    std::string compute_active_name(Region region) const;
    void resolve_config_unlocked(Region region);

    // 复制 backend 的共享所有权,未找到返回 nullptr。需调用方持锁。
    std::shared_ptr<WebSearchBackend> find_unlocked(const std::string& name);

    // 并发查询所选的两源，轮询合并并按 URL 去重。
    std::variant<SearchResponse, SearchError> search_parallel(
        std::string_view query,
        int limit,
        const std::atomic<bool>* abort,
        const NotifyFn& notify);

    const WebSearchConfig& cfg_;
    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<WebSearchBackend>> backends_;
    std::string active_;
    bool manual_selection_ = false;
    Region resolved_region_ = Region::Unknown;
};

// 注册 RSS、国内和全球网页来源。Bing CN 旧配置和 bochaai/tavily
// 占位配置会 LOG_WARN 并安全回退到 DDG。
void register_default_backends(BackendRouter& router, const WebSearchConfig& cfg);

} // namespace acecode::web_search
