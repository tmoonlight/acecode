#pragma once

#include <cstdint>
#include <string>

namespace acecode::desktop {

// 导航失败的归类。原生宿主先把平台错误折算成这几类,纯逻辑层只按类别决策,
// 于是 Windows 的 COREWEBVIEW2_WEB_ERROR_STATUS 与 macOS 的 NSError 能共用同一
// 套「中间回调不得变成最终失败」判定。
enum class BrowserNavigationFailure {
    kNone,            // 导航成功
    kCancelled,       // -999 / OPERATION_CANCELED:被新导航取代、重定向或主动取消
    kCertificate,     // 证书错误;放行事件可能先于也可能后于该回调到达
    kExternalHandoff, // 已交给操作系统的外部 scheme 之后的连接中止
    kTerminal,        // DNS、连接、代理、认证、进程等真实失败
};

enum class BrowserNavigationDecision {
    kIgnore,          // 陈旧回调或已处理原因:保持当前可见状态不动
    kHoldPending,     // 等待证书放行,不发布任何最终态
    kRetryOnce,       // 证书已放行但该次请求已失败,重新发起一次(全页最多一次)
    kRestorePrevious, // 恢复导航前的内容状态
    kPublishFailure,  // 当前导航的最终、不可覆盖失败
    kPublishSuccess,
};

// 每个原生 Browser 页面一份。只记录「当前导航身份 + 已处理原因」,不持有任何
// 平台对象,因此证书继续、取消、陈旧回调与外部 URI 交接的时序可以在没有真实
// WebView 的情况下单测。
//
// navigation_id 的含义按平台不同:Windows 用 WebView2 的 NavigationId,macOS 用
// WKNavigation 对象指针值。0 表示「身份未知」,一律按当前导航处理。
class AgentBrowserNavigationTracker {
public:
    void begin_navigation(std::uint64_t navigation_id);
    void note_external_handoff(std::uint64_t navigation_id);
    void close();

    BrowserNavigationDecision on_certificate_allowed(
        std::uint64_t navigation_id);
    BrowserNavigationDecision on_navigation_completed(
        std::uint64_t navigation_id, BrowserNavigationFailure failure);

    bool is_current(std::uint64_t navigation_id) const;
    bool closed() const { return closed_; }
    std::uint64_t current_navigation_id() const { return current_id_; }
    bool certificate_retry_used() const { return certificate_retry_used_; }
    bool pending_certificate_failure() const {
        return pending_certificate_failure_;
    }
    bool external_handoff_pending() const { return external_handoff_pending_; }

private:
    std::uint64_t current_id_ = 0;
    bool closed_ = false;
    bool certificate_allowed_ = false;
    bool certificate_retry_used_ = false;
    bool pending_certificate_failure_ = false;
    bool external_handoff_pending_ = false;
    // 重试会以新的 navigation id 重新开始一次导航,若不把「已用掉重试」这件事
    // 带过那次 begin_navigation,证书失败就会无限重试。
    bool carry_retry_marker_ = false;
};

// Windows:由 agent_browser_web_error_kind() 已经算好的 failure_kind 归类。
BrowserNavigationFailure classify_windows_navigation_failure(
    const std::string& failure_kind, bool external_handoff_pending);

// macOS:由 NSURLError 码归类。证书类错误在此归为 kTerminal —— 服务器信任挑战
// 已在认证回调里同步放行,仍然失败就是真实失败;归成 kCertificate 会让页面永远
// 停在 kHoldPending(macOS 没有对应的证书放行事件可以解除等待)。
BrowserNavigationFailure classify_darwin_navigation_failure(
    long long url_error_code, bool external_handoff_pending);

} // namespace acecode::desktop
