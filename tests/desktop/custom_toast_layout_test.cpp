// 自绘弹框的纯几何/缓动逻辑测试(可移植)。
//
// Win32 渲染部分(层窗口 + GDI + per-pixel alpha)只能在 Windows 实机验证,
// 但栈布局、屏幕锚定、DPI 缩放和三条缓动曲线都是纯算术,在这里锁住。

#include <gtest/gtest.h>

#include "desktop/custom_toast.hpp"

#include <vector>

using namespace acecode::desktop::custom_toast;

TEST(CustomToastScaling, IdentityAtBaseDpi) {
    EXPECT_EQ(scale_for_dpi(100, 96), 100);
}

TEST(CustomToastScaling, ScalesUpAndDown) {
    EXPECT_EQ(scale_for_dpi(100, 192), 200);
    EXPECT_EQ(scale_for_dpi(100, 144), 150);
    EXPECT_EQ(scale_for_dpi(100, 48), 50);
}

TEST(CustomToastScaling, ZeroDpiFallsBackToBase) {
    EXPECT_EQ(scale_for_dpi(37, 0), 37);
}

TEST(CustomToastEasing, EaseInStartsAtZeroAndEndsAtOne) {
    EXPECT_FLOAT_EQ(ease_in_position(0, 500), 0.0f);
    EXPECT_FLOAT_EQ(ease_in_position(500, 500), 1.0f);
    EXPECT_FLOAT_EQ(ease_in_position(9999, 500), 1.0f);
}

TEST(CustomToastEasing, EaseInDecelerates) {
    // 减速曲线:前半程走过的距离必须超过一半。
    EXPECT_GT(ease_in_position(250, 500), 0.5f);
    float previous = -1.0f;
    for (std::uint32_t t = 0; t <= 500; t += 25) {
        const float value = ease_in_position(t, 500);
        EXPECT_GE(value, previous);
        previous = value;
    }
}

TEST(CustomToastEasing, EaseOutAcceleratesAndEndsOpaque) {
    EXPECT_FLOAT_EQ(ease_out_position(0, 160), 0.0f);
    EXPECT_FLOAT_EQ(ease_out_position(160, 160), 1.0f);
    // 加速曲线:前半程走过的距离小于一半。
    EXPECT_LT(ease_out_position(80, 160), 0.5f);
}

TEST(CustomToastEasing, ZeroDurationIsAlreadyDone) {
    EXPECT_FLOAT_EQ(ease_in_position(0, 0), 1.0f);
    EXPECT_FLOAT_EQ(ease_out_position(0, 0), 1.0f);
    EXPECT_FLOAT_EQ(stack_collapse_position(0, 0), 1.0f);
}

TEST(CustomToastEasing, StackCollapseMatchesEaseInCurve) {
    for (std::uint32_t t = 0; t <= 400; t += 50) {
        EXPECT_FLOAT_EQ(stack_collapse_position(t, 400),
                        ease_in_position(t, 400));
    }
}

TEST(CustomToastDismissal, ClampsAbsurdSystemValues) {
    EXPECT_EQ(clamp_auto_dismiss_seconds(0), 6u);
    EXPECT_EQ(clamp_auto_dismiss_seconds(5), 6u);
    EXPECT_EQ(clamp_auto_dismiss_seconds(7), 7u);
    EXPECT_EQ(clamp_auto_dismiss_seconds(30), 30u);
    EXPECT_EQ(clamp_auto_dismiss_seconds(3600), 30u);
}

TEST(CustomToastPlacement, AnchorsToBottomRightOfWorkArea) {
    const ToastRect work_area{0, 0, 1920, 1040};
    const ToastPlacement placement =
        compute_toast_placement(work_area, 16, 16, 368, 96, 0, 1.0f);
    EXPECT_EQ(placement.width, 368);
    EXPECT_EQ(placement.height, 96);
    EXPECT_EQ(placement.x, 1920 - 16 - 368);
    EXPECT_EQ(placement.y, 1040 - 16 - 96);
}

TEST(CustomToastPlacement, RespectsWorkAreaOriginOnSecondaryMonitors) {
    // 副屏工作区不是从 0 开始的;锚点必须跟着屏幕走。
    const ToastRect work_area{1920, 0, 3840, 1080};
    const ToastPlacement placement =
        compute_toast_placement(work_area, 16, 16, 368, 96, 0, 1.0f);
    EXPECT_EQ(placement.x, 3840 - 16 - 368);
    EXPECT_EQ(placement.y, 1080 - 16 - 96);
}

TEST(CustomToastPlacement, StacksUpwardByVerticalOffset) {
    const ToastRect work_area{0, 0, 1920, 1040};
    const ToastPlacement lower =
        compute_toast_placement(work_area, 16, 16, 368, 96, 0, 1.0f);
    const ToastPlacement upper =
        compute_toast_placement(work_area, 16, 16, 368, 96, 108, 1.0f);
    EXPECT_EQ(upper.y, lower.y - 108);
    EXPECT_EQ(upper.x, lower.x);
}

TEST(CustomToastPlacement, EaseInKeepsTheRightEdgePinned) {
    const ToastRect work_area{0, 0, 1920, 1040};
    const ToastPlacement half =
        compute_toast_placement(work_area, 16, 16, 368, 96, 0, 0.5f);
    EXPECT_EQ(half.width, 184);
    // 右边缘不动:x + width 恒等于 work_area.right - margin。
    EXPECT_EQ(half.x + half.width, 1920 - 16);
}

TEST(CustomToastPlacement, EaseInClampsOutOfRangeProgress) {
    const ToastRect work_area{0, 0, 1920, 1040};
    EXPECT_EQ(compute_toast_placement(work_area, 16, 16, 368, 96, 0, -1.0f).width,
              0);
    EXPECT_EQ(compute_toast_placement(work_area, 16, 16, 368, 96, 0, 4.0f).width,
              368);
}

TEST(CustomToastStack, OffsetsAccumulateHeightsAndMargins) {
    const std::vector<int> heights{96, 120, 96};
    const std::vector<int> offsets = compute_stack_offsets(heights, 12);
    ASSERT_EQ(offsets.size(), 3u);
    EXPECT_EQ(offsets[0], 0);
    EXPECT_EQ(offsets[1], 96 + 12);
    EXPECT_EQ(offsets[2], 96 + 12 + 120 + 12);
}

TEST(CustomToastStack, EmptyStackHasNoOffsets) {
    EXPECT_TRUE(compute_stack_offsets({}, 12).empty());
}

TEST(CustomToastBody, FitsWholeLinesUpToTheCap) {
    EXPECT_EQ(fit_body_lines(60, 20, 3), 3);
    EXPECT_EQ(fit_body_lines(41, 20, 3), 2);
    EXPECT_EQ(fit_body_lines(19, 20, 3), 0);
    EXPECT_EQ(fit_body_lines(1000, 20, 3), 3);
}

TEST(CustomToastBody, DegenerateInputsReturnZero) {
    EXPECT_EQ(fit_body_lines(100, 0, 3), 0);
    EXPECT_EQ(fit_body_lines(100, 20, 0), 0);
}

TEST(CustomToastChrome, ExpandsAroundSurfaceWithoutMovingAnchor) {
    const auto geometry =
        compute_toast_chrome_geometry(1536, 928, 368, 96, 16);
    EXPECT_EQ(geometry.window_x, 1520);
    EXPECT_EQ(geometry.window_y, 912);
    EXPECT_EQ(geometry.window_width, 400);
    EXPECT_EQ(geometry.window_height, 128);
    EXPECT_EQ(geometry.surface_left, 16);
    EXPECT_EQ(geometry.surface_top, 16);
    EXPECT_EQ(geometry.surface_width, 368);
    EXPECT_EQ(geometry.surface_height, 96);
    // Surface screen origin is preserved.
    EXPECT_EQ(geometry.window_x + geometry.surface_left, 1536);
    EXPECT_EQ(geometry.window_y + geometry.surface_top, 928);
}

TEST(CustomToastChrome, ZeroInsetLeavesSurfaceUnchanged) {
    const auto geometry = compute_toast_chrome_geometry(10, 20, 100, 50, 0);
    EXPECT_EQ(geometry.window_x, 10);
    EXPECT_EQ(geometry.window_y, 20);
    EXPECT_EQ(geometry.window_width, 100);
    EXPECT_EQ(geometry.window_height, 50);
    EXPECT_EQ(geometry.surface_left, 0);
    EXPECT_EQ(geometry.surface_top, 0);
}

TEST(CustomToastShadow, CoverageIsOpaqueInsideAndTransparentOutside) {
    // Center of a 40x40 rounded rect (radius 8) is fully inside.
    EXPECT_EQ(toast_surface_coverage(
                  toast_rounded_rect_distance(20.0, 20.0, 40, 40, 8)),
              255);
    // Far outside has zero coverage.
    EXPECT_EQ(toast_surface_coverage(
                  toast_rounded_rect_distance(100.0, 100.0, 40, 40, 8)),
              0);
}

TEST(CustomToastShadow, AlphaFallsOffWithDistanceAndRespectsBlur) {
    EXPECT_EQ(toast_shadow_alpha(0.0, 12, 46), 46);
    EXPECT_EQ(toast_shadow_alpha(12.0, 12, 46), 0);
    EXPECT_EQ(toast_shadow_alpha(13.0, 12, 46), 0);
    EXPECT_EQ(toast_shadow_alpha(6.0, 12, 46), 12);  // 46 * (0.5)^2 = 11.5 -> 12
    EXPECT_EQ(toast_shadow_alpha(0.0, 0, 46), 0);
    EXPECT_EQ(toast_shadow_alpha(-2.0, 12, 46), 46);  // inside silhouette
}

TEST(CustomToastShadow, PlacementKeepsCardAnchoredWhenChromeExpands) {
    const ToastRect work_area{0, 0, 1920, 1040};
    const ToastPlacement surface =
        compute_toast_placement(work_area, 16, 16, 368, 96, 0, 1.0f);
    const auto chrome = compute_toast_chrome_geometry(
        surface.x, surface.y, surface.width, surface.height, 16);
    // Card surface still sits at the original bottom-right anchor.
    EXPECT_EQ(chrome.window_x + chrome.surface_left + chrome.surface_width,
              1920 - 16);
    EXPECT_EQ(chrome.window_y + chrome.surface_top + chrome.surface_height,
              1040 - 16);
}
