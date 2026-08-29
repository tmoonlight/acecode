#include <gtest/gtest.h>

#include "desktop/agent_browser_navigation_state.hpp"

using namespace acecode::desktop;

namespace {

// 触发场景:一次普通导航正常完成。
// 期望行为:发布成功态,不留下任何待处理标记。
TEST(AgentBrowserNavigationTracker, SuccessfulNavigationPublishesSuccess) {
    AgentBrowserNavigationTracker tracker;
    tracker.begin_navigation(1);
    EXPECT_EQ(tracker.on_navigation_completed(
                  1, BrowserNavigationFailure::kNone),
              BrowserNavigationDecision::kPublishSuccess);
    EXPECT_FALSE(tracker.pending_certificate_failure());
    EXPECT_FALSE(tracker.certificate_retry_used());
}

// 触发场景:DNS / 连接失败这类真实的最终失败。
// 期望行为:直接发布失败,宽松策略不能把真实故障吞成永久空白页。
TEST(AgentBrowserNavigationTracker, TerminalFailurePublishesFailure) {
    AgentBrowserNavigationTracker tracker;
    tracker.begin_navigation(7);
    EXPECT_EQ(tracker.on_navigation_completed(
                  7, BrowserNavigationFailure::kTerminal),
              BrowserNavigationDecision::kPublishFailure);
}

// 回归测试:录屏里刷新时闪白屏并停在「无法打开此页面」。
// 起因是 macOS 把 NSURLErrorCancelled (-999) 一律当成最终失败,而这个码在导航
// 被重定向或新导航取代时也会出现。
// 期望行为:取消类回调只恢复导航前的状态,不发布失败。
TEST(AgentBrowserNavigationTracker, CancelledNavigationRestoresPreviousState) {
    AgentBrowserNavigationTracker tracker;
    tracker.begin_navigation(11);
    EXPECT_EQ(tracker.on_navigation_completed(
                  11, BrowserNavigationFailure::kCancelled),
              BrowserNavigationDecision::kRestorePrevious);
}

// 触发场景:旧导航的失败回调在新导航已经开始之后才到达。
// 期望行为:按陈旧回调忽略,当前页面状态不被旧结果覆盖。
TEST(AgentBrowserNavigationTracker, StaleCallbackFromOlderNavigationIsIgnored) {
    AgentBrowserNavigationTracker tracker;
    tracker.begin_navigation(1);
    tracker.begin_navigation(2);
    EXPECT_EQ(tracker.on_navigation_completed(
                  1, BrowserNavigationFailure::kTerminal),
              BrowserNavigationDecision::kIgnore);
    EXPECT_EQ(tracker.current_navigation_id(), 2U);
}

// 触发场景:页面已经关闭,原生引擎的回调仍在路上。
// 期望行为:全部忽略,避免 handler 访问已失效的页面状态。
TEST(AgentBrowserNavigationTracker, CallbacksAfterCloseAreIgnored) {
    AgentBrowserNavigationTracker tracker;
    tracker.begin_navigation(3);
    tracker.close();
    EXPECT_EQ(tracker.on_navigation_completed(
                  3, BrowserNavigationFailure::kTerminal),
              BrowserNavigationDecision::kIgnore);
    EXPECT_EQ(tracker.on_certificate_allowed(3),
              BrowserNavigationDecision::kIgnore);
}

// 触发场景:WebView2 先报证书失败的 NavigationCompleted,随后才发出
// ServerCertificateErrorDetected。
// 期望行为:先挂起等待,证书放行后重新发起一次导航,期间不闪任何错误页。
TEST(AgentBrowserNavigationTracker,
     CertificateFailureBeforeAllowEventHoldsThenRetries) {
    AgentBrowserNavigationTracker tracker;
    tracker.begin_navigation(5);
    EXPECT_EQ(tracker.on_navigation_completed(
                  5, BrowserNavigationFailure::kCertificate),
              BrowserNavigationDecision::kHoldPending);
    EXPECT_TRUE(tracker.pending_certificate_failure());
    EXPECT_EQ(tracker.on_certificate_allowed(5),
              BrowserNavigationDecision::kRetryOnce);
    EXPECT_TRUE(tracker.certificate_retry_used());
}

// 触发场景:相反的事件顺序 —— 证书事件先到并已放行,证书失败的
// NavigationCompleted 随后才到。
// 期望行为:同样只重试一次,不需要先挂起。
TEST(AgentBrowserNavigationTracker,
     CertificateAllowBeforeFailureRetriesImmediately) {
    AgentBrowserNavigationTracker tracker;
    tracker.begin_navigation(9);
    EXPECT_EQ(tracker.on_certificate_allowed(9),
              BrowserNavigationDecision::kIgnore);
    EXPECT_EQ(tracker.on_navigation_completed(
                  9, BrowserNavigationFailure::kCertificate),
              BrowserNavigationDecision::kRetryOnce);
}

// 回归测试:重试会以新的 navigation id 重新开始一次导航。若 begin_navigation
// 无条件清空「已用掉重试」,证书始终失败的站点就会无限重试烧 CPU。
// 期望行为:重试标记跨过那一次 begin_navigation,第二次证书失败直接发布失败。
TEST(AgentBrowserNavigationTracker, CertificateRetryHappensAtMostOnce) {
    AgentBrowserNavigationTracker tracker;
    tracker.begin_navigation(5);
    EXPECT_EQ(tracker.on_navigation_completed(
                  5, BrowserNavigationFailure::kCertificate),
              BrowserNavigationDecision::kHoldPending);
    EXPECT_EQ(tracker.on_certificate_allowed(5),
              BrowserNavigationDecision::kRetryOnce);

    // 重试后的那次导航拿到新的 id。
    tracker.begin_navigation(6);
    EXPECT_TRUE(tracker.certificate_retry_used());
    EXPECT_EQ(tracker.on_navigation_completed(
                  6, BrowserNavigationFailure::kCertificate),
              BrowserNavigationDecision::kPublishFailure);
}

// 触发场景:重试后的导航成功,用户随后又访问另一个证书有问题的站点。
// 期望行为:重试预算按导航成功复位,不会因为上一次用过就永远不再重试。
TEST(AgentBrowserNavigationTracker, RetryBudgetResetsAfterSuccess) {
    AgentBrowserNavigationTracker tracker;
    tracker.begin_navigation(5);
    tracker.on_navigation_completed(5, BrowserNavigationFailure::kCertificate);
    tracker.on_certificate_allowed(5);
    tracker.begin_navigation(6);
    EXPECT_EQ(tracker.on_navigation_completed(
                  6, BrowserNavigationFailure::kNone),
              BrowserNavigationDecision::kPublishSuccess);

    tracker.begin_navigation(7);
    EXPECT_FALSE(tracker.certificate_retry_used());
    EXPECT_EQ(tracker.on_navigation_completed(
                  7, BrowserNavigationFailure::kCertificate),
              BrowserNavigationDecision::kHoldPending);
}

// 触发场景:登录流程跳到系统已注册的外部 SSO scheme,ACECode 已交给操作系统,
// 引擎随后报出取消 / 连接中止。
// 期望行为:恢复交接前的页面,不显示「无法打开此页面」。
TEST(AgentBrowserNavigationTracker, ExternalHandoffRestoresPrecedingPage) {
    AgentBrowserNavigationTracker tracker;
    tracker.begin_navigation(21);
    tracker.note_external_handoff(21);
    EXPECT_TRUE(tracker.external_handoff_pending());
    EXPECT_EQ(tracker.on_navigation_completed(
                  21, BrowserNavigationFailure::kExternalHandoff),
              BrowserNavigationDecision::kRestorePrevious);
    EXPECT_FALSE(tracker.external_handoff_pending());
}

// 触发场景:navigation id 未知(macOS 的 WKNavigation 可能为 nil)。
// 期望行为:按当前导航处理,不因为拿不到身份就整段失效。
TEST(AgentBrowserNavigationTracker, UnknownNavigationIdIsTreatedAsCurrent) {
    AgentBrowserNavigationTracker tracker;
    tracker.begin_navigation(4);
    EXPECT_TRUE(tracker.is_current(0));
    EXPECT_EQ(tracker.on_navigation_completed(
                  0, BrowserNavigationFailure::kTerminal),
              BrowserNavigationDecision::kPublishFailure);
}

// 触发场景:Windows 侧把 WebView2 的 failure_kind 折算成失败类别。
// 期望行为:证书单列;cancelled 归取消;connection_aborted 只有在存在待处理的
// 外部交接时才算交接,否则是真实的连接中止。
TEST(AgentBrowserNavigationTracker, WindowsFailureClassification) {
    EXPECT_EQ(classify_windows_navigation_failure("", false),
              BrowserNavigationFailure::kNone);
    EXPECT_EQ(classify_windows_navigation_failure("certificate", false),
              BrowserNavigationFailure::kCertificate);
    EXPECT_EQ(classify_windows_navigation_failure("cancelled", false),
              BrowserNavigationFailure::kCancelled);
    EXPECT_EQ(classify_windows_navigation_failure("connection_aborted", true),
              BrowserNavigationFailure::kExternalHandoff);
    EXPECT_EQ(classify_windows_navigation_failure("connection_aborted", false),
              BrowserNavigationFailure::kTerminal);
    EXPECT_EQ(classify_windows_navigation_failure("name_not_resolved", false),
              BrowserNavigationFailure::kTerminal);
}

// 触发场景:macOS 侧把 NSURLError 码折算成失败类别。
// 期望行为:-999 归取消(有待处理交接时归交接);证书类错误刻意归为最终失败 ——
// 服务器信任挑战已在认证回调里同步放行,macOS 没有可以解除 kHoldPending 的证书
// 放行事件,归成 kCertificate 会让页面永远停在加载中。
TEST(AgentBrowserNavigationTracker, DarwinFailureClassification) {
    EXPECT_EQ(classify_darwin_navigation_failure(-999, false),
              BrowserNavigationFailure::kCancelled);
    EXPECT_EQ(classify_darwin_navigation_failure(-999, true),
              BrowserNavigationFailure::kExternalHandoff);
    // -1202 = NSURLErrorServerCertificateHasBadDate
    EXPECT_EQ(classify_darwin_navigation_failure(-1202, false),
              BrowserNavigationFailure::kTerminal);
    // -1003 = NSURLErrorCannotFindHost
    EXPECT_EQ(classify_darwin_navigation_failure(-1003, false),
              BrowserNavigationFailure::kTerminal);
}

} // namespace
