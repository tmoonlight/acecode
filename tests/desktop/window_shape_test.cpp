// 覆盖 src/apps/desktop/window_shape.cpp:主题切角窗口的纯几何。
// 背景:主题包声明 "window": {"shape": "chamfer"} 时,Windows 桌面壳用 SetWindowRgn
// 把窗口四角按 45° 切掉,系统阴影随之消失,改由四块分层窗口按同一轮廓补画阴影。
// 区域与阴影都靠这里的同一个「像素是否在窗口内」判定,任何一边算偏都会在斜边上
// 露出一道缝(看到后面的桌面)或叠出一道深线。

#include <gtest/gtest.h>

#include "desktop/window_shape.hpp"

#include <cstdint>
#include <vector>

using acecode::desktop::ResizeZone;
using acecode::desktop::ShadowStrip;
using acecode::desktop::ShadowStyle;
using acecode::desktop::ShapeRect;
using acecode::desktop::chamfer_contains_pixel;
using acecode::desktop::chamfer_cut_px;
using acecode::desktop::chamfer_outside_distance;
using acecode::desktop::chamfer_region_rects;
using acecode::desktop::chamfer_row_inset;
using acecode::desktop::chamfer_shadow_strips;
using acecode::desktop::clamp_chamfer_cut;
using acecode::desktop::parse_window_shape_request;
using acecode::desktop::render_chamfer_shadow;
using acecode::desktop::chamfer_resize_zone;
using acecode::desktop::render_chamfer_grip_mask;

namespace {

std::uint8_t alpha_of(std::uint32_t pixel) { return static_cast<std::uint8_t>(pixel >> 24); }

}  // namespace

// 场景:切角大小随 DPI 缩放(与页面里 16 CSS px 的 HUD 斜边对齐)。
// 期望:96 DPI 下 16px,150% 下 24px,非法 DPI 按 96 处理,0 表示不切。
TEST(DesktopWindowShape, CutScalesWithDpi) {
    EXPECT_EQ(chamfer_cut_px(16, 96), 16);
    EXPECT_EQ(chamfer_cut_px(16, 144), 24);
    EXPECT_EQ(chamfer_cut_px(16, 0), 16);
    EXPECT_EQ(chamfer_cut_px(0, 144), 0);
}

// 场景:窗口被拖得极小(最小尺寸之外的兜底)。
// 期望:切角不超过短边的一半,上下两段切角不会交叉出负宽度的行。
TEST(DesktopWindowShape, CutClampsToHalfOfShortSide) {
    EXPECT_EQ(clamp_chamfer_cut(800, 600, 24), 24);
    EXPECT_EQ(clamp_chamfer_cut(30, 20, 24), 10);
    EXPECT_EQ(clamp_chamfer_cut(0, 600, 24), 0);
}

// 场景:每行左右让出的宽度。期望:按像素中心落在斜边内侧判定 —— 第 0 行让出 cut-1,
// 逐行减 1,切角以下为 0;底部与顶部上下对称。
TEST(DesktopWindowShape, RowInsetFollowsFortyFiveDegreeEdge) {
    const int cut = 4;
    const int height = 20;
    EXPECT_EQ(chamfer_row_inset(0, height, cut), 3);
    EXPECT_EQ(chamfer_row_inset(3, height, cut), 0);
    EXPECT_EQ(chamfer_row_inset(10, height, cut), 0);
    for (int y = 0; y < height; ++y) {
        EXPECT_EQ(chamfer_row_inset(y, height, cut), chamfer_row_inset(height - 1 - y, height, cut)) << y;
    }
}

// 场景:由矩形拼出的窗口区域。期望:矩形恰好覆盖判定为窗口内的像素,不多不少
// (逐像素对比),这样阴影的透明部分与区域严丝合缝。
TEST(DesktopWindowShape, RegionRectsCoverExactlyTheContainedPixels) {
    const int width = 37;
    const int height = 29;
    const int cut = 6;
    const auto rects = chamfer_region_rects(width, height, cut);
    ASSERT_EQ(rects.size(), static_cast<std::size_t>(2 * cut + 1));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int covered = 0;
            for (const auto& r : rects) {
                if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) ++covered;
            }
            EXPECT_EQ(covered, chamfer_contains_pixel(x, y, width, height, cut) ? 1 : 0) << x << "," << y;
        }
    }
    EXPECT_EQ(rects[cut], (ShapeRect{0, cut, width, height - cut}));
}

// 场景:到切角轮廓的距离(阴影浓淡按它衰减)。期望:形内为 0;正对边外 3px 就是 3;
// 斜边外沿法线方向 √2 处约为 1;离开窗口角点的距离按到斜边端点算。
TEST(DesktopWindowShape, OutsideDistanceMeasuresToTheOctagon) {
    const int width = 100;
    const int height = 80;
    const int cut = 10;
    EXPECT_DOUBLE_EQ(chamfer_outside_distance(50, 40, width, height, cut), 0.0);
    EXPECT_NEAR(chamfer_outside_distance(103, 40, width, height, cut), 3.0, 1e-9);
    EXPECT_NEAR(chamfer_outside_distance(50, -3, width, height, cut), 3.0, 1e-9);
    EXPECT_NEAR(chamfer_outside_distance(4, 4, width, height, cut), 1.4142135623730951, 1e-9);
    // 被切掉的角落本身在形外:原矩形的角点 (0,0) 到斜边的距离是 cut/√2。
    EXPECT_NEAR(chamfer_outside_distance(0, 0, width, height, cut), 10 / 1.4142135623730951, 1e-9);
}

// 场景:阴影窗口的摆放。期望:上下两块横跨整宽并向内覆盖切角高度(切掉的角落要露出
// 阴影),左右两块夹在中间;四块合起来覆盖窗口外一圈且彼此不重叠。
TEST(DesktopWindowShape, ShadowStripsSurroundTheWindow) {
    const auto strips = chamfer_shadow_strips(200, 120, 16, 14);
    ASSERT_EQ(strips.size(), 4u);
    EXPECT_EQ(strips[0], (ShadowStrip{-14, -14, 228, 30}));
    EXPECT_EQ(strips[1], (ShadowStrip{-14, 104, 228, 30}));
    EXPECT_EQ(strips[2], (ShadowStrip{-14, 16, 14, 88}));
    EXPECT_EQ(strips[3], (ShadowStrip{200, 16, 14, 88}));
    // 窗口矮到没有中段时只剩上下两块。
    EXPECT_EQ(chamfer_shadow_strips(200, 32, 16, 14).size(), 2u);
}

// 场景:阴影逐像素渲染。期望:窗口内(含斜边内侧)全透明,否则阴影窗口盖在主窗口
// 上会把内容压暗;贴着轮廓最浓,到外延宽度处衰减到 0;失焦时更淡;
// 辉光颜色按预乘 alpha 输出,任何分量都不超过 alpha(否则合成出错色)。
TEST(DesktopWindowShape, ShadowRenderIsTransparentInsideAndFadesOutside) {
    const int width = 200;
    const int height = 120;
    const int cut = 16;
    ShadowStyle style;
    style.margin = 14;
    style.offset_y = 1;
    style.glow_r = 0xFF;
    style.glow_g = 0x4D;
    style.glow_b = 0x57;
    style.glow_width = 3;
    const ShadowStrip top{-14, -14, 228, 30};
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(top.width) * top.height);
    render_chamfer_shadow(pixels.data(), top, width, height, cut, style);
    auto at = [&](int x, int y) { return pixels[static_cast<std::size_t>(y - top.y) * top.width + (x - top.x)]; };

    EXPECT_EQ(at(100, 5), 0u);            // 顶边以内
    EXPECT_EQ(at(16, 0), 0u);             // 斜边与顶边交点在形内
    EXPECT_GT(alpha_of(at(7, 7)), 100);   // 切掉的角落里紧贴斜边的像素,阴影 + 辉光最浓
    EXPECT_LT(alpha_of(at(0, 0)), 20);    // 原矩形角点离斜边 10px 以上,只剩很淡的阴影
    EXPECT_GT(alpha_of(at(100, -1)), 60); // 顶边外第一行
    EXPECT_EQ(alpha_of(at(100, -14)), 0); // 外延尽头衰减为 0
    EXPECT_GT(alpha_of(at(100, -1)), alpha_of(at(100, -8)));
    for (std::uint32_t pixel : pixels) {
        const std::uint8_t a = alpha_of(pixel);
        EXPECT_LE(static_cast<std::uint8_t>(pixel >> 16), a);
        EXPECT_LE(static_cast<std::uint8_t>(pixel >> 8), a);
        EXPECT_LE(static_cast<std::uint8_t>(pixel), a);
    }

    std::vector<std::uint32_t> inactive(pixels.size());
    style.active = false;
    render_chamfer_shadow(inactive.data(), top, width, height, cut, style);
    EXPECT_LT(alpha_of(inactive[static_cast<std::size_t>(13) * top.width + 114]),
              alpha_of(pixels[static_cast<std::size_t>(13) * top.width + 114]));
}

// 场景:切角窗口外侧的缩放把手(200x120 窗口,切角 16,外伸 8,角上沿边延伸 28)。
// 回归:切角后原来的角被切掉、隐形缩放边也去掉了,在原来的角上按下会点穿到后面的
// 窗口,用户反馈改变了拖角缩放的习惯。期望:切掉的缺口、窗口矩形外 8px 以内都能接住;
// 离角 28px 以内算角,其余算边;窗口内(交给页面)和 8px 以外都不接。
TEST(DesktopWindowShape, ResizeZonesCoverTheCutCornersAndOutsideBand) {
    const auto zone = [](int x, int y) { return chamfer_resize_zone(x, y, 200, 120, 16, 8, 28); };
    // 原来的四个角点(已切掉)与正外侧
    EXPECT_EQ(zone(0, 0), ResizeZone::TopLeft);
    EXPECT_EQ(zone(-8, -8), ResizeZone::TopLeft);
    EXPECT_EQ(zone(199, 0), ResizeZone::TopRight);
    EXPECT_EQ(zone(0, 119), ResizeZone::BottomLeft);
    EXPECT_EQ(zone(207, 127), ResizeZone::BottomRight);
    // 切口上贴着斜边的最后一个外侧像素仍是角,斜边内侧第一个像素交给页面
    EXPECT_EQ(zone(14, 0), ResizeZone::TopLeft);
    EXPECT_EQ(zone(15, 0), ResizeZone::None);
    // 沿边离角 28px 以内算角,再往里算边
    EXPECT_EQ(zone(27, -1), ResizeZone::TopLeft);
    EXPECT_EQ(zone(28, -1), ResizeZone::Top);
    EXPECT_EQ(zone(-1, 60), ResizeZone::Left);
    EXPECT_EQ(zone(200, 60), ResizeZone::Right);
    EXPECT_EQ(zone(100, 120), ResizeZone::Bottom);
    // 外伸 8px 以外与窗口内部都不接
    EXPECT_EQ(zone(-9, 60), ResizeZone::None);
    EXPECT_EQ(zone(100, -9), ResizeZone::None);
    EXPECT_EQ(zone(100, 60), ResizeZone::None);
}

// 场景:很小的窗口(40x30),角的延伸长度比半边还长。期望:角的范围收窄到半边,
// 四个角互不重叠,边的中点仍判为边。
TEST(DesktopWindowShape, ResizeCornersShrinkOnTinyWindows) {
    const auto zone = [](int x, int y) { return chamfer_resize_zone(x, y, 40, 30, 16, 8, 28); };
    EXPECT_EQ(zone(19, -1), ResizeZone::TopLeft);
    EXPECT_EQ(zone(20, -1), ResizeZone::TopRight);
    EXPECT_EQ(zone(-1, 14), ResizeZone::TopLeft);
    EXPECT_EQ(zone(-1, 15), ResizeZone::BottomLeft);
}

// 场景:把手窗口的像素。期望:接住鼠标的像素是 alpha 1 的黑色(分层窗口只让 alpha 0
// 的像素点穿,1/255 的黑看不出来),其余全透明;与 chamfer_resize_zone 逐像素一致。
TEST(DesktopWindowShape, GripMaskMatchesResizeZones) {
    const int width = 200;
    const int height = 120;
    const int cut = 16;
    const int grip = 8;
    for (const auto& strip : chamfer_shadow_strips(width, height, cut, grip)) {
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(strip.width) * strip.height, 0xDEADBEEFu);
        render_chamfer_grip_mask(pixels.data(), strip, width, height, cut, grip, 28);
        for (int row = 0; row < strip.height; ++row) {
            for (int column = 0; column < strip.width; ++column) {
                const bool hit = chamfer_resize_zone(strip.x + column, strip.y + row, width, height, cut, grip,
                                                     28) != ResizeZone::None;
                ASSERT_EQ(pixels[static_cast<std::size_t>(row) * strip.width + column], hit ? 0x01000000u : 0u)
                    << strip.x + column << "," << strip.y + row;
            }
        }
    }
}

// 场景:桥接参数(页面经 aceDesktop_setWindowShape 推送)。期望:只认 chamfer / none 与
// 可选的 #rrggbb 辉光色;其它形态一律拒绝,不猜 —— 页面笔误不能把窗口切成意外形状。
TEST(DesktopWindowShape, BridgeRequestParsing) {
    const auto chamfer = parse_window_shape_request(R"([{"shape":"chamfer","glow":"#2bd3f5"}])");
    ASSERT_TRUE(chamfer.has_value());
    EXPECT_TRUE(chamfer->chamfer);
    ASSERT_TRUE(chamfer->glow_rgb.has_value());
    EXPECT_EQ(*chamfer->glow_rgb, 0x2BD3F5u);

    const auto none = parse_window_shape_request(R"([{"shape":"none"}])");
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(none->chamfer);
    EXPECT_FALSE(none->glow_rgb.has_value());

    for (const char* bad : {R"([{"shape":"round"}])", R"([{"shape":"chamfer","glow":"red"}])",
                            R"([{"glow":"#ffffff"}])", R"(["chamfer"])", R"({"shape":"chamfer"})", "not json"}) {
        EXPECT_FALSE(parse_window_shape_request(bad).has_value()) << bad;
    }
}
