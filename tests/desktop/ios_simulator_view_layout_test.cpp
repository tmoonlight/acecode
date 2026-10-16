#include <gtest/gtest.h>

#include "desktop/ios_simulator_view_layout.hpp"

#include <cmath>

using namespace acecode::desktop;
using nlohmann::json;

namespace {

json sample_layout() {
    return {{"udid", "EBDA0D80-2BB4-4C9E-804C-5F5299CC1A2D"},
            {"developer_dir", "/Applications/Xcode.app/Contents/Developer"},
            {"x", 120},
            {"y", 64.6},
            {"width", 390},
            {"height", 844},
            {"visible", true},
            {"corner_radius", 42.5},
            {"layout_revision", 7},
            {"occlusion_rects", json::array({{{"x", 10}, {"y", 20}, {"width", 30}, {"height", 40}}})}};
}

} // namespace

// 场景:网页面板把屏幕区域交给 Desktop 原生视图。期望:字段按 CSS 像素解析,
// UDID / developer_dir / 遮挡矩形逐项校验,坏输入给出可读错误。
TEST(IosSimulatorViewLayout, ParsesTheBridgeLayout) {
    std::string error;
    const auto layout = parse_ios_simulator_view_layout(sample_layout(), &error);
    ASSERT_TRUE(layout) << error;
    EXPECT_EQ(layout->udid, "EBDA0D80-2BB4-4C9E-804C-5F5299CC1A2D");
    EXPECT_EQ(layout->frame.x, 120);
    EXPECT_EQ(layout->frame.y, 64);
    EXPECT_EQ(layout->frame.width, 390);
    EXPECT_EQ(layout->frame.height, 844);
    EXPECT_TRUE(layout->visible);
    EXPECT_DOUBLE_EQ(layout->corner_radius, 42.5);
    EXPECT_EQ(layout->layout_revision, 7u);
    ASSERT_EQ(layout->occlusion_rects.size(), 1u);
    EXPECT_EQ(layout->occlusion_rects[0].height, 40);
}

TEST(IosSimulatorViewLayout, RejectsMalformedLayouts) {
    std::string error;
    auto bad = sample_layout();
    bad["udid"] = "../../etc";
    EXPECT_FALSE(parse_ios_simulator_view_layout(bad, &error));
    EXPECT_NE(error.find("UDID"), std::string::npos);

    bad = sample_layout();
    bad["developer_dir"] = "relative/Developer";
    EXPECT_FALSE(parse_ios_simulator_view_layout(bad, &error));

    bad = sample_layout();
    bad["width"] = "wide";
    EXPECT_FALSE(parse_ios_simulator_view_layout(bad, &error));

    bad = sample_layout();
    bad["corner_radius"] = "round";
    EXPECT_FALSE(parse_ios_simulator_view_layout(bad, &error));

    bad = sample_layout();
    bad["layout_revision"] = -1;
    EXPECT_FALSE(parse_ios_simulator_view_layout(bad, &error));

    bad = sample_layout();
    bad["occlusion_rects"] = json::array();
    for (int index = 0; index < 65; ++index) bad["occlusion_rects"].push_back({{"x", 0}});
    EXPECT_FALSE(parse_ios_simulator_view_layout(bad, &error));

    EXPECT_FALSE(parse_ios_simulator_view_layout(json::array(), &error));
    EXPECT_FALSE(is_simulator_udid("EBDA0D80-2BB4-4C9E-804C-5F5299CC1A2"));
    EXPECT_FALSE(is_simulator_udid("EBDA0D80X2BB4-4C9E-804C-5F5299CC1A2D"));
    EXPECT_TRUE(is_simulator_udid("ebda0d80-2bb4-4c9e-804c-5f5299cc1a2d"));
}

// 场景:菜单 / 对话框部分盖住屏幕。期望:遮挡矩形裁到屏幕区域内,完全在外的丢弃。
TEST(IosSimulatorViewLayout, ClipsOcclusionRectsToTheFrame) {
    IosSimulatorViewLayout layout;
    layout.frame = {0, 0, 100, 200};
    layout.occlusion_rects = {{-10, -10, 30, 30}, {90, 190, 50, 50}, {200, 0, 10, 10}, {10, 10, 0, 5}};
    const auto clipped = clipped_occlusion_rects(layout);
    ASSERT_EQ(clipped.size(), 2u);
    EXPECT_EQ(clipped[0].x, 0);
    EXPECT_EQ(clipped[0].y, 0);
    EXPECT_EQ(clipped[0].width, 20);
    EXPECT_EQ(clipped[0].height, 20);
    EXPECT_EQ(clipped[1].x, 90);
    EXPECT_EQ(clipped[1].width, 10);
    EXPECT_EQ(clipped[1].height, 10);
}

// 场景:竖屏帧缓冲按界面方向转后等比放进屏幕区域。期望:图层保持帧缓冲本身的宽高比,
// 横屏时按转过来的尺寸适配,中心在区域中心。
TEST(IosSimulatorViewLayout, FitsThePortraitFramebufferForEachRotation) {
    auto portrait = fit_simulator_layer(390, 844, 1170, 2532, 0);
    EXPECT_NEAR(portrait.width, 390, 1e-6);
    EXPECT_NEAR(portrait.height, 844, 1e-6);
    EXPECT_NEAR(portrait.center_x, 195, 1e-6);
    EXPECT_NEAR(portrait.center_y, 422, 1e-6);

    auto landscape = fit_simulator_layer(844, 390, 1170, 2532, 90);
    EXPECT_EQ(landscape.rotation, 90);
    EXPECT_NEAR(landscape.width, 390, 1e-6) << "unrotated bounds stay portrait";
    EXPECT_NEAR(landscape.height, 844, 1e-6);

    auto letterboxed = fit_simulator_layer(400, 400, 1170, 2532, 270);
    EXPECT_EQ(letterboxed.rotation, 270);
    EXPECT_NEAR(letterboxed.height, 400, 1e-6) << "the turned width fills the box";
    EXPECT_NEAR(letterboxed.width, 400 * 1170.0 / 2532.0, 1e-6);

    EXPECT_EQ(fit_simulator_layer(100, 100, 10, 20, -90).rotation, 270);
    EXPECT_EQ(fit_simulator_layer(100, 100, 10, 20, 45).rotation, 0);
    EXPECT_EQ(fit_simulator_layer(0, 100, 10, 20, 0).width, 0);
}

TEST(IosSimulatorViewLayout, LayerAngleTurnsClockwiseInFlippedGeometry) {
    const double pi = std::acos(-1.0);
    EXPECT_NEAR(simulator_layer_angle_radians(90, true), pi / 2, 1e-9);
    EXPECT_NEAR(simulator_layer_angle_radians(90, false), -pi / 2, 1e-9);
    EXPECT_NEAR(simulator_layer_angle_radians(270, true), 3 * pi / 2, 1e-9);
    EXPECT_NEAR(simulator_layer_angle_radians(-90, true), 3 * pi / 2, 1e-9);
    EXPECT_NEAR(simulator_layer_angle_radians(0, false), 0, 1e-9);
}
