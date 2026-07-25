// notifications.{hpp,cpp} 公共生命周期契约的可移植测试。
//
// Windows 实现在系统 toast(WinToast)与自绘弹框之间按策略二选一，macOS Desktop
// 使用 UserNotifications，其他平台保留 no-op 桩。后端选择判定表在
// notification_backend_policy_test.cpp。
// 这里覆盖不依赖真实通知中心的默认值、失败降级与重复清理行为。
// Payload 解析、Unicode 截断和独立 activation 路由见
// notifications_native_test.cpp。

#include <gtest/gtest.h>

#include "desktop/notifications.hpp"

using namespace acecode::desktop;

TEST(DesktopNotificationsPayload, DefaultsAreEmpty) {
    NotifyPayload p;
    EXPECT_EQ(p.id, "");
    EXPECT_EQ(p.workspace_hash, "");
    EXPECT_EQ(p.session_id, "");
    EXPECT_EQ(p.title, "");
    EXPECT_EQ(p.body, "");
}

TEST(DesktopNotificationsPayload, AssignAllFieldsCopiesCleanly) {
    NotifyPayload p;
    p.id = "n-1";
    p.workspace_hash = "ws-abc";
    p.session_id = "s-xyz";
    p.title = "需要你回答 · 测试会话";
    p.body  = "请确认是否继续";
    NotifyPayload copy = p;
    EXPECT_EQ(copy.id, "n-1");
    EXPECT_EQ(copy.workspace_hash, "ws-abc");
    EXPECT_EQ(copy.session_id, "s-xyz");
    EXPECT_EQ(copy.title, "需要你回答 · 测试会话");
    EXPECT_EQ(copy.body, "请确认是否继续");
}

TEST(DesktopNotificationsLifecycle, InitWithoutPlatformIdentityIsSafe) {
    shutdown_notifications();
    NotificationInitOptions options;
#if defined(__APPLE__) || defined(_WIN32)
    // macOS: a command-line test runner may or may not be wrapped in a bundle
    // by the generator.
    // Windows: a missing AppUserModelID no longer means "no notifications" —
    // the self-drawn renderer covers that case — but it still needs a usable
    // window station, which a headless runner may not have.
    // Either result is valid; lifecycle safety is the contract.
    const bool initialized = init_notifications(options);
    if (initialized) shutdown_notifications();
#else
    EXPECT_FALSE(init_notifications(options));
    NotifyPayload p;
    p.title = "x";
    p.body = "y";
    EXPECT_FALSE(show_notification(p));
    EXPECT_NO_THROW(shutdown_notifications());
#endif
}

#ifdef _WIN32
TEST(DesktopNotificationsLifecycle, SystemBackendIsNotSilentlyReplaced) {
    // backend="system" 是显式选择:没有 AUMID 就没有系统 toast,此时不擅自
    // 换成自绘窗口,而是老老实实报告初始化失败。
    shutdown_notifications();
    NotificationInitOptions options;
    options.backend = "system";
    EXPECT_FALSE(init_notifications(options));
    NotifyPayload p;
    p.title = "x";
    p.body = "y";
    EXPECT_FALSE(show_notification(p));
    EXPECT_NO_THROW(shutdown_notifications());
}
#endif

TEST(DesktopNotificationsLifecycle, RepeatedShutdownIsSafe) {
    EXPECT_NO_THROW(shutdown_notifications());
    EXPECT_NO_THROW(shutdown_notifications());
    EXPECT_NO_THROW(shutdown_notifications());
}

TEST(DesktopNotificationsLifecycle, SetClickHandlerNullptrIsSafe) {
    EXPECT_NO_THROW(set_click_handler(nullptr));
    EXPECT_NO_THROW(dispatch_notification_activation(NotifyPayload{}));
}

TEST(DesktopNotificationsLifecycle, SetClickHandlerAcceptsLambda) {
    int seen = 0;
    set_click_handler([&seen](const NotifyPayload&) {
        ++seen;
    });
    dispatch_notification_activation(NotifyPayload{});
    EXPECT_EQ(seen, 1);
    // 清理避免后续测试残留
    shutdown_notifications();
}
