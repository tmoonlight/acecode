#include "desktop/window_size.hpp"

#include <gtest/gtest.h>

namespace acecode::desktop {
namespace {

TEST(WindowSizeTest, PreservesRequestedSizeWhenItFits) {
    const auto size = clamp_window_size_to_work_area({1280, 820}, {1920, 1040});

    EXPECT_EQ(size.width, 1280);
    EXPECT_EQ(size.height, 820);
}

TEST(WindowSizeTest, ClampsEachOversizedDimensionIndependently) {
    const auto width_limited = clamp_window_size_to_work_area({1280, 700}, {1024, 740});
    EXPECT_EQ(width_limited.width, 1024);
    EXPECT_EQ(width_limited.height, 700);

    const auto both_limited = clamp_window_size_to_work_area({1280, 820}, {1024, 720});
    EXPECT_EQ(both_limited.width, 1024);
    EXPECT_EQ(both_limited.height, 720);
}

TEST(WindowSizeTest, NormalizesDegenerateInputsToPositiveDimensions) {
    const auto size = clamp_window_size_to_work_area({0, -20}, {-1, 0});

    EXPECT_EQ(size.width, 1);
    EXPECT_EQ(size.height, 1);
}

TEST(WindowSizeTest, SafeMarginsPreservePreferredSizeWhenItFits) {
    const auto size =
        fit_desktop_window_to_safe_work_area({1280, 820}, {1920, 1040}, 96);

    EXPECT_EQ(size.width, 1280);
    EXPECT_EQ(size.height, 820);
}

TEST(WindowSizeTest, SafeMarginsConstrainShortDisplayHeight) {
    const auto size =
        fit_desktop_window_to_safe_work_area({1280, 820}, {1536, 731}, 96);

    EXPECT_EQ(size.width, 1280);
    EXPECT_EQ(size.height, 611);
}

TEST(WindowSizeTest, SafeMarginsScaleWithMonitorDpi) {
    const auto size =
        fit_desktop_window_to_safe_work_area({1280, 820}, {1536, 731}, 144);

    EXPECT_EQ(size.width, 1280);
    EXPECT_EQ(size.height, 551);
}

TEST(WindowSizeTest, SafeMarginsNormalizeDegenerateWorkArea) {
    const auto size =
        fit_desktop_window_to_safe_work_area({1280, 820}, {0, -20}, 96);

    EXPECT_EQ(size.width, 1);
    EXPECT_EQ(size.height, 1);
}

TEST(WindowSizeTest, FinalRectExcludesBottomTaskbarAfterHighDpiScaling) {
    const auto rect = fit_centered_desktop_window_rect_to_safe_work_area(
        {1920, 1230}, {0, 0, 1920, 1040}, 144);

    EXPECT_EQ(rect.left, 60);
    EXPECT_EQ(rect.top, 90);
    EXPECT_EQ(rect.right, 1860);
    EXPECT_EQ(rect.bottom, 950);
    EXPECT_LT(rect.bottom, 1040);
}

TEST(WindowSizeTest, FinalRectCentersInsideOffsetMonitorWorkArea) {
    const auto rect = fit_centered_desktop_window_rect_to_safe_work_area(
        {1280, 820}, {-1920, 40, 0, 1080}, 96);

    EXPECT_EQ(rect.left, -1600);
    EXPECT_EQ(rect.top, 150);
    EXPECT_EQ(rect.right, -320);
    EXPECT_EQ(rect.bottom, 970);
}

TEST(WindowSizeTest, FinalRectNormalizesDegenerateWorkArea) {
    const auto rect = fit_centered_desktop_window_rect_to_safe_work_area(
        {1280, 820}, {50, 70, 40, 60}, 96);

    EXPECT_EQ(rect.left, 50);
    EXPECT_EQ(rect.top, 70);
    EXPECT_EQ(rect.right, 51);
    EXPECT_EQ(rect.bottom, 71);
}

// 下面几条的隐形缩放边按 96 DPI 下 Windows 无标题栏窗口的实际值:左/右/下各 8 像素,顶边 0。
constexpr WindowFrameInsets kInvisibleFrame96{8, 0, 8, 8};
constexpr DesktopWindowMonitor kPrimary1080p{{0, 0, 1920, 1040}, 96};

SavedDesktopWindowBounds saved_bounds(WindowRect rect, int dpi = 96) {
    SavedDesktopWindowBounds bounds;
    bounds.rect = rect;
    bounds.dpi = dpi;
    return bounds;
}

// 场景:上次窗口完整落在原显示器工作区内,显示器与缩放都没变。
// 期望:位置和尺寸原样恢复,不再居中、不再收进安全边距。
TEST(WindowSizeTest, SavedWindowInsideWorkAreaIsRestoredExactly) {
    const auto rect = place_saved_desktop_window(
        saved_bounds({100, 80, 1380, 900}), kInvisibleFrame96, kPrimary1080p, kPrimary1080p);

    EXPECT_EQ(rect.left, 100);
    EXPECT_EQ(rect.top, 80);
    EXPECT_EQ(rect.right, 1380);
    EXPECT_EQ(rect.bottom, 900);
}

// 场景:窗口铺满工作区(贴边分屏或手动拉满),外框比工作区多出左/右/下的隐形缩放边。
// 期望:按去掉隐形边后的可见窗口判断,仍算完整可见,原样恢复。
// 回归:只拿外框比时这类窗口每次启动都会被缩小并居中。
TEST(WindowSizeTest, SavedWindowFillingWorkAreaCountsInvisibleFrameAsVisible) {
    const auto rect = place_saved_desktop_window(
        saved_bounds({-8, 0, 1928, 1048}), kInvisibleFrame96, kPrimary1080p, kPrimary1080p);

    EXPECT_EQ(rect.left, -8);
    EXPECT_EQ(rect.top, 0);
    EXPECT_EQ(rect.right, 1928);
    EXPECT_EQ(rect.bottom, 1048);
}

// 场景:上次窗口有一部分拖出了屏幕右边。
// 期望:不原样恢复,走原有逻辑:保留上次尺寸(放得下就不缩),在回退显示器上居中。
TEST(WindowSizeTest, SavedWindowPartlyOffscreenFallsBackToCenteredClamp) {
    const auto rect = place_saved_desktop_window(
        saved_bounds({1500, 80, 2780, 900}), kInvisibleFrame96, kPrimary1080p, kPrimary1080p);

    EXPECT_EQ(rect.left, 320);
    EXPECT_EQ(rect.top, 110);
    EXPECT_EQ(rect.right, 1600);
    EXPECT_EQ(rect.bottom, 930);
}

// 场景:标题栏所在的顶边越出工作区 1 像素(顶边没有隐形缩放边)。
// 期望:视为超出可视范围,走回退;否则窗口顶部可能拖不回来。
TEST(WindowSizeTest, SavedWindowAboveWorkAreaTopFallsBack) {
    const auto rect = place_saved_desktop_window(
        saved_bounds({100, -1, 1380, 819}), kInvisibleFrame96, kPrimary1080p, kPrimary1080p);

    EXPECT_EQ(rect.left, 320);
    EXPECT_EQ(rect.top, 110);
}

// 场景:上次窗口所在的副屏已经拔掉(找不到显示器时调用方传 DPI 0 的空显示器),
// 而且上次尺寸比现在的主屏还大。
// 期望:按原有 clamp 逻辑收进安全工作区(左右各 40、上下各 60 DIP)并在主屏居中。
TEST(WindowSizeTest, SavedWindowOnMissingMonitorIsClampedToFallbackMonitor) {
    const DesktopWindowMonitor missing{{0, 0, 0, 0}, 0};
    const auto rect = place_saved_desktop_window(
        saved_bounds({-2600, 0, 0, 1500}), kInvisibleFrame96, missing, kPrimary1080p);

    EXPECT_EQ(rect.left, 40);
    EXPECT_EQ(rect.top, 60);
    EXPECT_EQ(rect.right, 1880);
    EXPECT_EQ(rect.bottom, 980);
}

// 场景:位置仍在屏幕内,但用户把显示器缩放从 100% 改成了 150%。
// 期望:不按旧像素原样恢复(窗口会显得小一圈),而是把尺寸按 DPI 换算后居中。
TEST(WindowSizeTest, SavedWindowAfterScaleChangeIsRescaledAndCentered) {
    const DesktopWindowMonitor scaled{{0, 0, 2880, 1560}, 144};
    const auto rect = place_saved_desktop_window(
        saved_bounds({100, 80, 1380, 900}, 96), kInvisibleFrame96, scaled, scaled);

    EXPECT_EQ(rect.right - rect.left, 1920);
    EXPECT_EQ(rect.bottom - rect.top, 1230);
    EXPECT_EQ(rect.left, 480);
    EXPECT_EQ(rect.top, 165);
}

// 场景:副屏在主屏左侧,工作区是负坐标,上次窗口完整在副屏上。
// 期望:负坐标同样按工作区判断,原样恢复。
TEST(WindowSizeTest, SavedWindowOnLeftSecondaryMonitorIsRestoredExactly) {
    const DesktopWindowMonitor left_monitor{{-1920, 40, 0, 1080}, 96};
    const auto rect = place_saved_desktop_window(
        saved_bounds({-1800, 100, -400, 1000}), kInvisibleFrame96, left_monitor, kPrimary1080p);

    EXPECT_EQ(rect.left, -1800);
    EXPECT_EQ(rect.top, 100);
    EXPECT_EQ(rect.right, -400);
    EXPECT_EQ(rect.bottom, 1000);
}

} // namespace
} // namespace acecode::desktop
