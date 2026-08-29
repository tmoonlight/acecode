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

} // namespace
} // namespace acecode::desktop
