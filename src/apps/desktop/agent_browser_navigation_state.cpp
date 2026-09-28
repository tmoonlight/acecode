#include "agent_browser_navigation_state.hpp"

namespace acecode::desktop {

namespace {

// NSURLErrorCancelled。这里用字面量而不是 Foundation 常量,是为了让本文件在
// 非 Apple 平台上也能编译进单测目标。
constexpr long long kDarwinErrorCancelled = -999;

} // namespace

bool AgentBrowserNavigationTracker::is_current(
    std::uint64_t navigation_id) const {
    if (closed_) return false;
    if (navigation_id == 0 || current_id_ == 0) return true;
    return navigation_id == current_id_;
}

void AgentBrowserNavigationTracker::begin_navigation(
    std::uint64_t navigation_id) {
    if (closed_) return;
    if (navigation_id != 0 && navigation_id == current_id_) return;
    current_id_ = navigation_id;
    certificate_allowed_ = false;
    pending_certificate_failure_ = false;
    external_handoff_pending_ = false;
    certificate_retry_used_ = carry_retry_marker_;
    carry_retry_marker_ = false;
}

void AgentBrowserNavigationTracker::note_external_handoff(
    std::uint64_t navigation_id) {
    if (!is_current(navigation_id)) return;
    external_handoff_pending_ = true;
}

void AgentBrowserNavigationTracker::close() {
    closed_ = true;
    pending_certificate_failure_ = false;
    external_handoff_pending_ = false;
}

BrowserNavigationDecision
AgentBrowserNavigationTracker::on_certificate_allowed(
    std::uint64_t navigation_id) {
    if (!is_current(navigation_id)) return BrowserNavigationDecision::kIgnore;
    certificate_allowed_ = true;
    if (pending_certificate_failure_ && !certificate_retry_used_) {
        pending_certificate_failure_ = false;
        certificate_retry_used_ = true;
        carry_retry_marker_ = true;
        return BrowserNavigationDecision::kRetryOnce;
    }
    return BrowserNavigationDecision::kIgnore;
}

BrowserNavigationDecision
AgentBrowserNavigationTracker::on_navigation_completed(
    std::uint64_t navigation_id, BrowserNavigationFailure failure) {
    if (!is_current(navigation_id)) return BrowserNavigationDecision::kIgnore;
    switch (failure) {
    case BrowserNavigationFailure::kNone:
        certificate_allowed_ = false;
        certificate_retry_used_ = false;
        pending_certificate_failure_ = false;
        external_handoff_pending_ = false;
        carry_retry_marker_ = false;
        return BrowserNavigationDecision::kPublishSuccess;
    case BrowserNavigationFailure::kExternalHandoff:
        external_handoff_pending_ = false;
        return BrowserNavigationDecision::kRestorePrevious;
    case BrowserNavigationFailure::kCancelled:
        external_handoff_pending_ = false;
        return BrowserNavigationDecision::kRestorePrevious;
    case BrowserNavigationFailure::kCertificate:
        if (certificate_retry_used_) {
            return BrowserNavigationDecision::kPublishFailure;
        }
        if (certificate_allowed_) {
            pending_certificate_failure_ = false;
            certificate_retry_used_ = true;
            carry_retry_marker_ = true;
            return BrowserNavigationDecision::kRetryOnce;
        }
        pending_certificate_failure_ = true;
        return BrowserNavigationDecision::kHoldPending;
    case BrowserNavigationFailure::kTerminal:
    default:
        return BrowserNavigationDecision::kPublishFailure;
    }
}

BrowserNavigationFailure classify_windows_navigation_failure(
    const std::string& failure_kind, bool external_handoff_pending) {
    if (failure_kind.empty()) return BrowserNavigationFailure::kNone;
    if (failure_kind == "certificate") {
        return BrowserNavigationFailure::kCertificate;
    }
    if (failure_kind == "cancelled") {
        return external_handoff_pending
            ? BrowserNavigationFailure::kExternalHandoff
            : BrowserNavigationFailure::kCancelled;
    }
    if (failure_kind == "connection_aborted") {
        // 外部 URI 交接后 WebView2 报的就是 CONNECTION_ABORTED;没有待处理的
        // 交接时它是真实的连接中止。
        return external_handoff_pending
            ? BrowserNavigationFailure::kExternalHandoff
            : BrowserNavigationFailure::kTerminal;
    }
    return BrowserNavigationFailure::kTerminal;
}

BrowserNavigationFailure classify_darwin_navigation_failure(
    long long url_error_code, bool external_handoff_pending) {
    if (url_error_code == kDarwinErrorCancelled) {
        return external_handoff_pending
            ? BrowserNavigationFailure::kExternalHandoff
            : BrowserNavigationFailure::kCancelled;
    }
    return BrowserNavigationFailure::kTerminal;
}

} // namespace acecode::desktop
