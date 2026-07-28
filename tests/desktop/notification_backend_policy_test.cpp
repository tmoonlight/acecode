// Windows 弹框后端选择策略的可移植测试。
//
// 背景:WinToast 在部分 Win10 机器上 initialize/showToast 全部返回成功,弹框
// 却永远不出现 —— AUMID 快捷方式缺失、通知被系统或组策略关掉、被"优化"工具
// 禁用通知平台。这些都读不出错误码,只能靠注册表信号提前判定,改走自绘渲染。
// 这里覆盖判定表本身;注册表探测与 GDI 渲染在 Windows 端实机验证。

#include <gtest/gtest.h>

#include "desktop/notification_backend_policy.hpp"

using namespace acecode::desktop;

namespace {

SystemToastSignals healthy_signals() {
    SystemToastSignals signals;
    signals.platform_supported = true;
    return signals;
}

} // namespace

TEST(NotificationBackendPreference, ParsesKnownValues) {
    EXPECT_EQ(parse_notification_backend_preference("auto"),
              NotificationBackendPreference::Auto);
    EXPECT_EQ(parse_notification_backend_preference("system"),
              NotificationBackendPreference::System);
    EXPECT_EQ(parse_notification_backend_preference("custom"),
              NotificationBackendPreference::Custom);
}

TEST(NotificationBackendPreference, IsCaseAndWhitespaceInsensitive) {
    EXPECT_EQ(parse_notification_backend_preference("  Custom "),
              NotificationBackendPreference::Custom);
    EXPECT_EQ(parse_notification_backend_preference("SYSTEM"),
              NotificationBackendPreference::System);
}

TEST(NotificationBackendPreference, AcceptsAliases) {
    EXPECT_EQ(parse_notification_backend_preference("wintoast"),
              NotificationBackendPreference::System);
    EXPECT_EQ(parse_notification_backend_preference("builtin"),
              NotificationBackendPreference::Custom);
}

TEST(NotificationBackendPreference, UnknownValueFallsBack) {
    EXPECT_EQ(parse_notification_backend_preference(""),
              NotificationBackendPreference::Auto);
    EXPECT_EQ(parse_notification_backend_preference(
                  "nope", NotificationBackendPreference::Custom),
              NotificationBackendPreference::Custom);
}

TEST(NotificationBackendPreference, RoundTripsThroughConfigValue) {
    for (auto preference : {NotificationBackendPreference::Auto,
                            NotificationBackendPreference::System,
                            NotificationBackendPreference::Custom}) {
        EXPECT_EQ(parse_notification_backend_preference(
                      notification_backend_preference_value(preference)),
                  preference);
    }
}

TEST(NotificationBackendDecision, AutoPrefersSystemWhenNothingSuppressesIt) {
    const auto decision = decide_notification_backend(
        NotificationBackendPreference::Auto, healthy_signals());
    EXPECT_EQ(decision.choice, NotificationBackendChoice::System);
    EXPECT_FALSE(decision.reason.empty());
}

TEST(NotificationBackendDecision, AutoFallsBackWhenPlatformUnsupported) {
    SystemToastSignals signals = healthy_signals();
    signals.platform_supported = false;
    EXPECT_EQ(decide_notification_backend(NotificationBackendPreference::Auto,
                                          signals)
                  .choice,
              NotificationBackendChoice::Custom);
}

TEST(NotificationBackendDecision, AutoFallsBackWhenToastsDisabledSystemWide) {
    SystemToastSignals signals = healthy_signals();
    signals.toasts_enabled_globally = false;
    EXPECT_EQ(decide_notification_backend(NotificationBackendPreference::Auto,
                                          signals)
                  .choice,
              NotificationBackendChoice::Custom);
}

TEST(NotificationBackendDecision, AutoFallsBackWhenAppToastsDisabled) {
    SystemToastSignals signals = healthy_signals();
    signals.app_toasts_enabled = false;
    EXPECT_EQ(decide_notification_backend(NotificationBackendPreference::Auto,
                                          signals)
                  .choice,
              NotificationBackendChoice::Custom);
}

TEST(NotificationBackendDecision, AutoFallsBackOnGroupPolicy) {
    SystemToastSignals signals = healthy_signals();
    signals.policy_disabled = true;
    EXPECT_EQ(decide_notification_backend(NotificationBackendPreference::Auto,
                                          signals)
                  .choice,
              NotificationBackendChoice::Custom);
}

TEST(NotificationBackendDecision, AutoFallsBackAfterRuntimeDeliveryFailure) {
    SystemToastSignals signals = healthy_signals();
    signals.runtime_delivery_failed = true;
    EXPECT_EQ(decide_notification_backend(NotificationBackendPreference::Auto,
                                          signals)
                  .choice,
              NotificationBackendChoice::Custom);
}

TEST(NotificationBackendDecision, CustomIgnoresEverySystemSignal) {
    SystemToastSignals signals = healthy_signals();
    EXPECT_EQ(decide_notification_backend(NotificationBackendPreference::Custom,
                                          signals)
                  .choice,
              NotificationBackendChoice::Custom);

    signals.platform_supported = false;
    signals.policy_disabled = true;
    EXPECT_EQ(decide_notification_backend(NotificationBackendPreference::Custom,
                                          signals)
                  .choice,
              NotificationBackendChoice::Custom);
}

TEST(NotificationBackendDecision, SystemIsNeverSilentlyReplacedByCustom) {
    // 用户显式要 OS 通知(想要通知中心归档)时不擅自换成自绘窗口。
    SystemToastSignals signals = healthy_signals();
    signals.platform_supported = false;
    const auto decision = decide_notification_backend(
        NotificationBackendPreference::System, signals);
    EXPECT_EQ(decision.choice, NotificationBackendChoice::None);
}

TEST(NotificationBackendDecision, SystemIgnoresSuppressionSignals) {
    // 注册表信号只是启发式;显式选 system 时仍然尝试投递。
    SystemToastSignals signals = healthy_signals();
    signals.toasts_enabled_globally = false;
    signals.app_toasts_enabled = false;
    EXPECT_EQ(decide_notification_backend(NotificationBackendPreference::System,
                                          signals)
                  .choice,
              NotificationBackendChoice::System);
}

TEST(NotificationBackendChoiceName, CoversEveryChoice) {
    EXPECT_STREQ(notification_backend_choice_name(
                     NotificationBackendChoice::None), "none");
    EXPECT_STREQ(notification_backend_choice_name(
                     NotificationBackendChoice::System), "system");
    EXPECT_STREQ(notification_backend_choice_name(
                     NotificationBackendChoice::Custom), "custom");
}
