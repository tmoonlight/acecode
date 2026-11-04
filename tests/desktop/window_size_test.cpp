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

// 下面是 macOS / Linux 用的逻辑坐标口径:没有隐形缩放边,dpi 恒为 96。
constexpr DesktopWindowMonitor kLogicalPrimary{{0, 25, 1440, 900}, kLogicalDesktopWindowDpi};
constexpr DesktopWindowMonitor kLogicalRight{{1440, 0, 3360, 1080}, kLogicalDesktopWindowDpi};

// 场景:窗口横跨主屏与右侧副屏,大部分落在副屏上。
// 期望:按重叠面积选中副屏(与 Windows 的 MonitorFromRect 同口径),而不是第一个有交集的主屏。
TEST(WindowSizeTest, MonitorForRectPicksLargestOverlap) {
    const auto monitor = desktop_monitor_for_rect({1300, 100, 2300, 700},
                                                  {kLogicalPrimary, kLogicalRight});

    ASSERT_TRUE(monitor.has_value());
    EXPECT_EQ(monitor->work_area.left, 1440);
}

// 场景:上次窗口所在的显示器已经拔掉,保存的位置不和任何现存显示器重叠。
// 期望:找不到显示器;调用方据此走居中回退。
TEST(WindowSizeTest, MonitorForRectIsEmptyWhenNothingOverlaps) {
    EXPECT_FALSE(desktop_monitor_for_rect({-2000, 0, -1000, 600},
                                          {kLogicalPrimary, kLogicalRight}).has_value());
    EXPECT_FALSE(desktop_monitor_for_rect({0, 25, 800, 600}, {}).has_value());
}

// 场景:macOS / Linux X11 上次窗口完整落在副屏工作区里。
// 期望:原位恢复,restored 为 true(macOS 据此不再调用 [window center])。
TEST(WindowSizeTest, LogicalSavedWindowOnSecondaryMonitorIsRestored) {
    const auto placement = place_saved_logical_desktop_window(
        saved_bounds({1600, 100, 2880, 920}), {kLogicalPrimary, kLogicalRight},
        kLogicalPrimary.work_area);

    EXPECT_TRUE(placement.restored);
    EXPECT_EQ(placement.rect.left, 1600);
    EXPECT_EQ(placement.rect.top, 100);
    EXPECT_EQ(placement.rect.right, 2880);
    EXPECT_EQ(placement.rect.bottom, 920);
}

// 场景:窗口顶边钻到了 macOS 菜单栏下面(工作区从 y=25 开始),或横跨两块屏。
// 期望:都算超出可视范围,保留上次尺寸在回退工作区居中。
TEST(WindowSizeTest, LogicalSavedWindowOutsideWorkAreaFallsBackToCentered) {
    const auto under_menu_bar = place_saved_logical_desktop_window(
        saved_bounds({100, 10, 1100, 710}), {kLogicalPrimary, kLogicalRight},
        kLogicalPrimary.work_area);
    EXPECT_FALSE(under_menu_bar.restored);
    EXPECT_EQ(under_menu_bar.rect.left, 220);
    EXPECT_EQ(under_menu_bar.rect.top, 112);
    EXPECT_EQ(under_menu_bar.rect.right, 1220);
    EXPECT_EQ(under_menu_bar.rect.bottom, 812);

    const auto spanning = place_saved_logical_desktop_window(
        saved_bounds({1000, 100, 2000, 800}), {kLogicalPrimary, kLogicalRight},
        kLogicalPrimary.work_area);
    EXPECT_FALSE(spanning.restored);
    EXPECT_EQ(spanning.rect.right - spanning.rect.left, 1000);
    EXPECT_EQ(spanning.rect.bottom - spanning.rect.top, 700);
}

// 场景:Wayland 下读不到也设不了窗口位置,调用方传空的显示器列表;
// 上次尺寸还比现在的工作区大。
// 期望:不原位恢复,只沿用尺寸,按原有逻辑收进安全区(左右 40、上下 60)并居中。
TEST(WindowSizeTest, LogicalSavedWindowWithoutPositionKeepsOnlyClampedSize) {
    const auto placement = place_saved_logical_desktop_window(
        saved_bounds({0, 0, 1600, 1000}), {}, {0, 0, 1366, 768});

    EXPECT_FALSE(placement.restored);
    EXPECT_EQ(placement.rect.left, 40);
    EXPECT_EQ(placement.rect.top, 60);
    EXPECT_EQ(placement.rect.right, 1326);
    EXPECT_EQ(placement.rect.bottom, 708);
}

// 场景:主屏 1440×900 点,AppKit 给出的窗口 frame 是左下原点、y 向上。
// 期望:换成左上原点后 top = 主屏高度 - (y + 高),再换回去得到同一个 frame。
TEST(WindowSizeTest, CocoaFrameConvertsToTopLeftAndBack) {
    const auto rect = top_left_rect_from_cocoa_frame({100, 200, 800, 600}, 900);
    EXPECT_EQ(rect.left, 100);
    EXPECT_EQ(rect.top, 100);
    EXPECT_EQ(rect.right, 900);
    EXPECT_EQ(rect.bottom, 700);

    const auto frame = cocoa_frame_from_top_left_rect(rect, 900);
    EXPECT_DOUBLE_EQ(frame.x, 100);
    EXPECT_DOUBLE_EQ(frame.y, 200);
    EXPECT_DOUBLE_EQ(frame.width, 800);
    EXPECT_DOUBLE_EQ(frame.height, 600);
}

// 场景:副屏摆在主屏上方,AppKit 里它的 y 大于主屏高度。
// 期望:换算后 top 为负,与 Windows 的副屏负坐标同样处理;来回换算不丢位置。
TEST(WindowSizeTest, CocoaFrameAbovePrimaryScreenHasNegativeTop) {
    const auto rect = top_left_rect_from_cocoa_frame({200, 1100, 1000, 700}, 900);
    EXPECT_EQ(rect.top, -900);
    EXPECT_EQ(rect.bottom, -200);

    const auto frame = cocoa_frame_from_top_left_rect(rect, 900);
    EXPECT_DOUBLE_EQ(frame.y, 1100);
    EXPECT_DOUBLE_EQ(frame.height, 700);
}

// 场景:Retina 屏上窗口 frame 可能落在半点上。
// 期望:四条边各自四舍五入,宽高误差不超过 1 点,不会因为先取整原点再加宽高而越来越偏。
TEST(WindowSizeTest, CocoaFractionalFrameRoundsEachEdge) {
    const auto rect = top_left_rect_from_cocoa_frame({10.4, 20.6, 799.2, 600.0}, 900.0);
    EXPECT_EQ(rect.left, 10);
    EXPECT_EQ(rect.right, 810);
    EXPECT_EQ(rect.top, 279);
    EXPECT_EQ(rect.bottom, 879);
}

} // namespace
} // namespace acecode::desktop
