// 桌面宠物(像素办公室 demo)窗口几何的纯逻辑测试(可移植)。
//
// Win32 透明窗口 + WebView2 只能在 Windows 实机看效果,但下面这些决定
// 「窗口多大、贴在哪、哪里能点」的算术在这里锁住:
//   1. 整数倍缩放随 DPI 取值(场景 344×252 像素,整数倍才不糊);
//   2. 小屏幕上缩放逐级回退,窗口不超过工作区一半;
//   3. 贴工作区右下角并留 8 个逻辑像素边距(工作区已扣任务栏,可能不从 0 开始);
//   4. 用户拖动过之后 DPI 变化只换尺寸、不跳回角落,且不被推出屏幕;
//   5. 命中区域是房间外包六边形:透明角落不接点击,房间内部都能点到;
//   6. 用户缩放的上下限(逻辑 0.5 倍到工作区 90%);
//   7. 用户调过的大小跨 DPI 按逻辑倍数换算;
//   8. 滚轮 / 把手缩放时锚点不动,放不下时推回工作区;
//   9. 小数倍缩放时命中区域按四舍五入换算;
//  10. 页面上报的浮层(成员列表 / 提示 / 状态气泡)解析:坏数据整条丢弃、坐标夹紧、条数有上限;
//  11. 点击命中判定(macOS 用它切换点击穿透):房间、控制条、浮层命中,透明角落穿透。

#include <gtest/gtest.h>

#include "desktop/desktop_pet_layout.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <vector>

using namespace acecode::desktop;

namespace {

// 射线法判断点是否在多边形内部,只给测试用。
bool inside(const std::vector<DesktopPetPoint>& polygon, int x, int y) {
    bool result = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const auto& a = polygon[i];
        const auto& b = polygon[j];
        if ((a.y > y) != (b.y > y)) {
            const double cross = a.x + static_cast<double>(y - a.y) * (b.x - a.x) / (b.y - a.y);
            if (x < cross) result = !result;
        }
    }
    return result;
}

constexpr DesktopPetRect kFullHd{0, 0, 1920, 1040};

} // namespace

// 场景 1:常见 DPI 下的缩放倍数。
// 触发:工作区足够大(1920×1040),DPI 取 96/120/144/168/192。
// 期望:round(dpi/96 × 1.25) → 1/2/2/2/3。96 DPI 时 1.25 四舍五入为 1(344px 宽),
// 192 DPI 时 2.5 进位为 3(逻辑宽度约 516px),都在「约 430 逻辑像素」附近。
TEST(DesktopPetLayout, ScaleFollowsDpiWithIntegerSteps) {
    EXPECT_EQ(desktop_pet_scale(96, 3840, 2160), 1);
    EXPECT_EQ(desktop_pet_scale(120, 3840, 2160), 2);
    EXPECT_EQ(desktop_pet_scale(144, 3840, 2160), 2);
    EXPECT_EQ(desktop_pet_scale(168, 3840, 2160), 2);
    EXPECT_EQ(desktop_pet_scale(192, 3840, 2160), 3);
}

// 场景 1b:拿不到 DPI(0 或负数)时按 96 处理,不能算出 0 倍窗口。
TEST(DesktopPetLayout, InvalidDpiFallsBackToBase) {
    EXPECT_EQ(desktop_pet_scale(0, 1920, 1040), 1);
    EXPECT_EQ(desktop_pet_scale(-5, 1920, 1040), 1);
}

// 场景 2:小工作区上缩放回退。
// 触发:192 DPI 本该取 3 倍(1032×756),但工作区只有 1366×728。
// 期望:3 倍宽 1032 > 683(一半宽)→ 退到 2 倍(688 > 683 仍超)→ 1 倍。
// 回退到 1 为止,再小也不会出现 0 倍。
TEST(DesktopPetLayout, ScaleShrinksToFitHalfOfWorkArea) {
    EXPECT_EQ(desktop_pet_scale(192, 1366, 728), 1);
    EXPECT_EQ(desktop_pet_scale(192, 1600, 1080), 2);   // 688 ≤ 800、504 ≤ 450
    EXPECT_EQ(desktop_pet_scale(192, 100, 100), 1);
}

// 场景 3:贴右下角。
// 触发:96 DPI、工作区 1920×1040(底部 40px 是任务栏,已被扣掉)。
// 期望:窗口 344×252,右边距与下边距都是 8px → x = 1920-344-8,y = 1040-252-8。
TEST(DesktopPetLayout, PlacesAtBottomRightWithMargin) {
    const auto placement = place_desktop_pet(kFullHd, 96);
    EXPECT_EQ(placement.scale, 1);
    EXPECT_EQ(placement.window.width, 344);
    EXPECT_EQ(placement.window.height, 252);
    EXPECT_EQ(placement.window.x, 1920 - 344 - 8);
    EXPECT_EQ(placement.window.y, 1040 - 252 - 8);
}

// 场景 3b:工作区不从原点开始(任务栏在左侧/顶部,或副屏在主屏左边为负坐标)。
// 触发:工作区 {-1920, 60, 1920, 1020},144 DPI(边距 8×1.5=12px,2 倍窗口 688×504)。
// 期望:右下角按工作区自身的右/下边算,而不是按屏幕原点。
TEST(DesktopPetLayout, PlacementRespectsOffsetWorkArea) {
    const auto placement = place_desktop_pet({-1920, 60, 1920, 1020}, 144);
    EXPECT_EQ(placement.scale, 2);
    EXPECT_EQ(placement.window.x, -1920 + 1920 - 688 - 12);
    EXPECT_EQ(placement.window.y, 60 + 1020 - 504 - 12);
}

// 场景 4:拖动过之后换 DPI(比如拖到另一块屏)。
// 触发 a:1080p 工作区,左上角 (100, 120),新 DPI 144 → 2 倍 688×504,原位放得下。
// 触发 b:4K 工作区 3840×2080,左上角 (3500, 1900),新 DPI 192 → 3 倍 1032×756。
// 期望:左上角尽量不动;原位置放不下新尺寸时往回推,整个窗口留在工作区内
// (x ≤ 3840-1032,y ≤ 2080-756),不会一半跑到屏幕外。
TEST(DesktopPetLayout, ResizeKeepsOriginButStaysInsideWorkArea) {
    const auto kept = resize_desktop_pet({100, 120, 344, 252}, 144, kFullHd);
    EXPECT_EQ(kept.scale, 2);
    EXPECT_EQ(kept.window.x, 100);
    EXPECT_EQ(kept.window.y, 120);
    EXPECT_EQ(kept.window.width, 688);

    const auto pushed = resize_desktop_pet({3500, 1900, 344, 252}, 192, {0, 0, 3840, 2080});
    EXPECT_EQ(pushed.scale, 3);
    EXPECT_EQ(pushed.window.x, 3840 - 1032);
    EXPECT_EQ(pushed.window.y, 2080 - 756);
}

// 场景 5:命中区域 = 房间外包六边形。
// 触发:1 倍缩放下取多边形,再按 2 倍取一次。
// 期望:
//   - 6 个顶点、全部落在 344×252 画布内;
//   - 画布四个透明角(原来的草地)都在多边形外 → 点击穿透到后面的窗口;
//   - 房间地板中心、左墙门口、后墙窗户、最前面的柜台这些有内容的点都在多边形内;
//   - 2 倍缩放的顶点恰好是 1 倍的两倍(窗口区域按设备像素给)。
TEST(DesktopPetLayout, HitPolygonCoversRoomAndLeavesCornersOpen) {
    const auto polygon = desktop_pet_hit_polygon(1);
    ASSERT_EQ(polygon.size(), 6u);
    for (const auto& point : polygon) {
        EXPECT_GE(point.x, 0);
        EXPECT_LE(point.x, kDesktopPetSceneWidth);
        EXPECT_GE(point.y, 0);
        EXPECT_LE(point.y, kDesktopPetSceneHeight);
    }

    EXPECT_FALSE(inside(polygon, 2, 2)) << "左上角是透明草地,不该接点击";
    EXPECT_FALSE(inside(polygon, 340, 2)) << "右上角是透明草地";
    EXPECT_FALSE(inside(polygon, 2, 248)) << "左下角是透明草地";
    EXPECT_FALSE(inside(polygon, 340, 248)) << "右下角是透明草地";

    EXPECT_TRUE(inside(polygon, 172, 162)) << "地板中间";
    EXPECT_TRUE(inside(polygon, 30, 142)) << "左墙上的门";
    EXPECT_TRUE(inside(polygon, 240, 92)) << "后墙的窗户";
    EXPECT_TRUE(inside(polygon, 188, 237)) << "房间最前面的柜台";

    const auto doubled = desktop_pet_hit_polygon(2);
    ASSERT_EQ(doubled.size(), polygon.size());
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        EXPECT_EQ(doubled[i].x, polygon[i].x * 2);
        EXPECT_EQ(doubled[i].y, polygon[i].y * 2);
    }
}

// 场景 6:缩放上下限。
// 触发:96 DPI、1920×1040 工作区,分别请求 0.1 倍、10 倍、2.3 倍和 NaN。
// 期望:下限 = 逻辑 0.5 倍(96 DPI 下就是 0.5);上限 = min(1920×0.9/344, 1040×0.9/252),
// 这里高度先顶到(约 4.2545);中间值原样返回;NaN 当成下限,不能让窗口尺寸变成非法值。
// 192 DPI 时下限跟着变成 1.0(仍是逻辑 0.5 倍,172 逻辑像素宽)。
TEST(DesktopPetLayout, ClampKeepsScaleBetweenHalfAndNinetyPercent) {
    EXPECT_DOUBLE_EQ(clamp_desktop_pet_scale(0.1, 96, kFullHd), 0.5);
    EXPECT_NEAR(clamp_desktop_pet_scale(10.0, 96, kFullHd), 1040 * 0.9 / 252, 1e-9);
    EXPECT_DOUBLE_EQ(clamp_desktop_pet_scale(2.3, 96, kFullHd), 2.3);
    EXPECT_DOUBLE_EQ(clamp_desktop_pet_scale(std::nan(""), 96, kFullHd), 0.5);
    EXPECT_DOUBLE_EQ(clamp_desktop_pet_scale(0.2, 192, {0, 0, 3840, 2080}), 1.0);
}

// 场景 7:用户调过大小(逻辑 1.25 倍)后在 144 DPI 下摆放。
// 期望:设备倍数 = 1.25 × 144/96 = 1.875,窗口 645×473(472.5 四舍五入),
// 仍贴右下角,边距 12px;请求超大时被夹到上限,高度正好是工作区的 90%(936)。
TEST(DesktopPetLayout, PreferredLogicalScaleFollowsDpi) {
    const auto placement = place_desktop_pet(kFullHd, 144, 1.25);
    EXPECT_DOUBLE_EQ(placement.scale, 1.875);
    EXPECT_EQ(placement.window.width, 645);
    EXPECT_EQ(placement.window.height, 473);
    EXPECT_EQ(placement.window.x, 1920 - 645 - 12);
    EXPECT_EQ(placement.window.y, 1040 - 473 - 12);

    const auto huge = place_desktop_pet(kFullHd, 96, 20.0);
    EXPECT_EQ(huge.window.height, 936);
    EXPECT_LE(huge.window.width, 1920);
}

// 场景 8:以锚点缩放。
// 触发:窗口 {1000, 500, 344×252} 放大到 2 倍。
// 期望:锚点 (1, 1)(停靠右下角时)→ 右下角 (1344, 720) 不动,新窗口左上角 (656, 280);
// 锚点 (0.5, 0.5)(鼠标在正中滚轮)→ 中心 (1172, 610) 不动,左上角 (828, 390);
// 锚点 (0, 0) 但右下方放不下 → 整个窗口推回工作区内,不会有一半跑出屏幕。
TEST(DesktopPetLayout, ScaleAroundAnchorKeepsPointFixed) {
    const DesktopPetRect current{1000, 500, 344, 252};
    const auto corner = scale_desktop_pet(current, 2.0, 1.0, 1.0, kFullHd);
    EXPECT_EQ(corner.window.width, 688);
    EXPECT_EQ(corner.window.height, 504);
    EXPECT_EQ(corner.window.x, 656);
    EXPECT_EQ(corner.window.y, 248);

    const auto center = scale_desktop_pet(current, 2.0, 0.5, 0.5, kFullHd);
    EXPECT_EQ(center.window.x, 828);
    EXPECT_EQ(center.window.y, 374);

    const auto pushed = scale_desktop_pet({1500, 700, 344, 252}, 2.0, 0.0, 0.0, kFullHd);
    EXPECT_EQ(pushed.window.x, 1920 - 688);
    EXPECT_EQ(pushed.window.y, 1040 - 504);
}

// 场景 9:小数倍缩放(1.5 倍)时的命中区域。
// 期望:每个顶点等于 1 倍顶点 × 1.5 后四舍五入,窗口区域和画面按同一比例对齐。
TEST(DesktopPetLayout, HitPolygonSupportsFractionalScale) {
    const auto base = desktop_pet_hit_polygon(1.0);
    const auto scaled = desktop_pet_hit_polygon(1.5);
    ASSERT_EQ(scaled.size(), base.size());
    for (std::size_t i = 0; i < base.size(); ++i) {
        EXPECT_EQ(scaled[i].x, static_cast<int>(std::lround(base[i].x * 1.5)));
        EXPECT_EQ(scaled[i].y, static_cast<int>(std::lround(base[i].y * 1.5)));
    }
}

// 场景:页面发来 overlay 消息,rect 是成员列表 / 提示的外包框,bubbles 是状态气泡列表,
// 其中混着越界坐标、NaN 以外的非数字、宽度为 0 的矩形和超量条目。
// 期望:越界坐标夹到 [0,1];非数字、空矩形整条丢弃;rect=null 时只取气泡;
// 总数不超过 kDesktopPetMaxOverlays。
// 回归:修复前原生端只认一个 rect,冒出墙顶的气泡被窗口区域裁掉、也点不到。
TEST(DesktopPetLayout, OverlayMessageKeepsValidRectsOnly) {
    const auto overlays = desktop_pet_overlays_from_message(nlohmann::json{
        {"type", "overlay"},
        {"rect", {0.1, 0.2, 0.5, 0.6}},
        {"bubbles", nlohmann::json::array({
            nlohmann::json::array({-0.2, 0.1, 0.3, 1.4}),
            nlohmann::json::array({0.4, "x", 0.5, 0.6}),
            nlohmann::json::array({0.4, 0.4, 0.4, 0.6}),
            nlohmann::json::array({0.6, 0.1, 0.7})})}});
    ASSERT_EQ(overlays.size(), 2u);
    EXPECT_DOUBLE_EQ(overlays[1][0], 0.0) << "左边越界夹到 0";
    EXPECT_DOUBLE_EQ(overlays[1][3], 1.0) << "下边越界夹到 1";

    const auto bubbles_only = desktop_pet_overlays_from_message(nlohmann::json{
        {"type", "overlay"}, {"rect", nullptr},
        {"bubbles", nlohmann::json::array({nlohmann::json::array({0.2, 0.2, 0.3, 0.3})})}});
    ASSERT_EQ(bubbles_only.size(), 1u);

    nlohmann::json many = nlohmann::json::array();
    for (int i = 0; i < 40; ++i) many.push_back({0.1, 0.1, 0.2, 0.2});
    EXPECT_EQ(desktop_pet_overlays_from_message({{"bubbles", many}}).size(), kDesktopPetMaxOverlays);
    EXPECT_TRUE(desktop_pet_overlays_from_message(nlohmann::json::array()).empty());
}

// 场景:1.25 倍(430×315)窗口,依次点:房间中央、顶部控制条、左上透明角落、
// 左上角落里一个上报过的气泡、窗口外。
// 期望:房间 / 控制条 / 气泡命中;没有浮层的透明角落与窗口外不命中(点击应穿透)。
TEST(DesktopPetLayout, HitTestCoversRoomToolbarAndOverlays) {
    const double scale = 1.25;
    const int width = 430, height = 315;
    const std::vector<DesktopPetOverlay> none;
    EXPECT_TRUE(desktop_pet_hit_test(scale, width, height, none, 215, 200)) << "房间中央";
    EXPECT_FALSE(desktop_pet_hit_test(scale, width, height, none, 400, 10)) << "隐藏的控制条穿透";
    const auto controls = desktop_pet_overlays_from_message(nlohmann::json{
        {"controls", {0.0, 0.0, 1.0, 32.0 / kDesktopPetSceneHeight}}});
    EXPECT_TRUE(desktop_pet_hit_test(scale, width, height, controls, 400, 10)) << "可见控制条";
    const auto hidden = desktop_pet_overlays_from_message(nlohmann::json{{"controls", nullptr}});
    EXPECT_FALSE(desktop_pet_hit_test(scale, width, height, hidden, 400, 10)) << "移出后控制条穿透";
    EXPECT_FALSE(desktop_pet_hit_test(scale, width, height, none, 6, 60)) << "左上透明角落";
    EXPECT_FALSE(desktop_pet_hit_test(scale, width, height, none, 6, 310)) << "左下透明角落";
    const std::vector<DesktopPetOverlay> bubble{{0.0, 0.15, 0.1, 0.25}};
    EXPECT_TRUE(desktop_pet_hit_test(scale, width, height, bubble, 6, 60)) << "角落里的气泡";
    EXPECT_FALSE(desktop_pet_hit_test(scale, width, height, bubble, -1, 60)) << "窗口外";
    EXPECT_FALSE(desktop_pet_hit_test(scale, width, height, bubble, 430, 60)) << "右边界外";
}

TEST(DesktopPetLayout, ControlsOverlayUsesValidationAndSharesOverlayLimit) {
    EXPECT_TRUE(desktop_pet_overlays_from_message(nlohmann::json{{"controls", {0, "bad", 1, 1}}}).empty());
    EXPECT_TRUE(desktop_pet_overlays_from_message(nlohmann::json{{"controls", {0, 0, 0, 1}}}).empty());
    nlohmann::json many = nlohmann::json::array();
    for (int i = 0; i < 40; ++i) many.push_back({0.1, 0.1, 0.2, 0.2});
    const auto overlays = desktop_pet_overlays_from_message(nlohmann::json{
        {"controls", {-0.1, 0.0, 1.2, 0.1}}, {"rect", {0.0, 0.2, 0.3, 0.4}}, {"bubbles", many}});
    ASSERT_EQ(overlays.size(), kDesktopPetMaxOverlays);
    EXPECT_EQ(overlays.front(), (DesktopPetOverlay{0.0, 0.0, 1.0, 0.1}));
    EXPECT_EQ(overlays[1], (DesktopPetOverlay{0.0, 0.2, 0.3, 0.4}));
}
