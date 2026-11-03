// 场景:桌面版主窗口位置跨启动保存在 state.json 的 desktop_window 段。
#include <gtest/gtest.h>
#include "desktop/window_bounds_state.hpp"
#include "test_support/utils/state_file_fixture.hpp"

#include <nlohmann/json.hpp>

namespace {

using acecode::desktop::SavedDesktopWindowBounds;
using acecode::desktop::desktop_window_bounds_json;
using acecode::desktop::parse_desktop_window_bounds;
using acecode::desktop::read_desktop_window_bounds;
using acecode::desktop::write_desktop_window_bounds;
using acecode::test_support::write_raw;

class DesktopWindowBoundsStateTest : public acecode::test_support::StateFileTest {};

nlohmann::json valid_bounds_json() {
    return {{"left", -1800}, {"top", 100}, {"right", -400}, {"bottom", 1000},
            {"dpi", 144}, {"maximized", true}};
}

// 场景:退出时写入窗口位置,下次启动读回。
// 期望:四条边、DPI、最大化标记逐项一致;state.json 里其它字段不被覆盖。
TEST_F(DesktopWindowBoundsStateTest, RoundTripKeepsOtherStateKeys) {
    acecode::write_state_flag("some_flag", true);
    SavedDesktopWindowBounds bounds;
    bounds.rect = {-1800, 100, -400, 1000};
    bounds.dpi = 144;
    bounds.maximized = true;
    write_desktop_window_bounds(bounds);

    const auto loaded = read_desktop_window_bounds();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->rect.left, -1800);
    EXPECT_EQ(loaded->rect.top, 100);
    EXPECT_EQ(loaded->rect.right, -400);
    EXPECT_EQ(loaded->rect.bottom, 1000);
    EXPECT_EQ(loaded->dpi, 144);
    EXPECT_TRUE(loaded->maximized);
    EXPECT_TRUE(acecode::read_state_flag("some_flag"));
}

// 场景:首次安装或旧版本升级上来,state.json 里还没有 desktop_window。
// 期望:读出空值,启动走原有的默认尺寸 + 居中逻辑。
TEST_F(DesktopWindowBoundsStateTest, MissingSectionReadsEmpty) {
    write_raw(path_, R"({"last_active_workspace_hash": "abc"})");
    EXPECT_FALSE(read_desktop_window_bounds().has_value());
}

// 场景:旧数据或手改出来的坏值(缺字段、类型不对、矩形退化、DPI / 坐标离谱)。
// 期望:一律当作没有保存过,不让坏数据把窗口摆到奇怪的位置。
TEST(DesktopWindowBoundsParseTest, RejectsMalformedValues) {
    EXPECT_FALSE(parse_desktop_window_bounds(nlohmann::json::array()).has_value());

    auto missing = valid_bounds_json();
    missing.erase("bottom");
    EXPECT_FALSE(parse_desktop_window_bounds(missing).has_value());

    auto wrong_type = valid_bounds_json();
    wrong_type["left"] = "10";
    EXPECT_FALSE(parse_desktop_window_bounds(wrong_type).has_value());

    auto fractional = valid_bounds_json();
    fractional["top"] = 1.5;
    EXPECT_FALSE(parse_desktop_window_bounds(fractional).has_value());

    auto empty_width = valid_bounds_json();
    empty_width["right"] = -1800;
    EXPECT_FALSE(parse_desktop_window_bounds(empty_width).has_value());

    auto inverted_height = valid_bounds_json();
    inverted_height["bottom"] = 50;
    EXPECT_FALSE(parse_desktop_window_bounds(inverted_height).has_value());

    // DPI 合理范围 48–960(100%–500% 缩放再留余量),0 与过大值都是坏数据。
    auto zero_dpi = valid_bounds_json();
    zero_dpi["dpi"] = 0;
    EXPECT_FALSE(parse_desktop_window_bounds(zero_dpi).has_value());

    auto huge_dpi = valid_bounds_json();
    huge_dpi["dpi"] = 5000;
    EXPECT_FALSE(parse_desktop_window_bounds(huge_dpi).has_value());

    // 坐标超出百万像素不可能来自真实显示器布局,换算时还可能溢出。
    auto huge_coordinate = valid_bounds_json();
    huge_coordinate["right"] = 5'000'000'000LL;
    EXPECT_FALSE(parse_desktop_window_bounds(huge_coordinate).has_value());
}

// 场景:maximized 字段缺失或不是布尔值(例如更早的手写配置)。
// 期望:位置照常恢复,只是不以最大化显示。
TEST(DesktopWindowBoundsParseTest, MaximizedDefaultsToFalse) {
    auto without_flag = valid_bounds_json();
    without_flag.erase("maximized");
    const auto parsed = parse_desktop_window_bounds(without_flag);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->maximized);

    auto string_flag = valid_bounds_json();
    string_flag["maximized"] = "true";
    const auto parsed_string = parse_desktop_window_bounds(string_flag);
    ASSERT_TRUE(parsed_string.has_value());
    EXPECT_FALSE(parsed_string->maximized);
}

// 场景:序列化后的 JSON 再解析。
// 期望:与原值一致,写盘格式与读盘校验同口径。
TEST(DesktopWindowBoundsParseTest, JsonRoundTrip) {
    SavedDesktopWindowBounds bounds;
    bounds.rect = {10, 20, 1290, 840};
    bounds.dpi = 96;
    const auto parsed = parse_desktop_window_bounds(desktop_window_bounds_json(bounds));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->rect.left, 10);
    EXPECT_EQ(parsed->rect.top, 20);
    EXPECT_EQ(parsed->rect.right, 1290);
    EXPECT_EQ(parsed->rect.bottom, 840);
    EXPECT_EQ(parsed->dpi, 96);
    EXPECT_FALSE(parsed->maximized);
}

} // namespace
