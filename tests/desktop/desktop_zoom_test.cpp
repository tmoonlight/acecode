// 场景:桌面版主窗口 Ctrl+滚轮页面缩放的原生侧状态(aceDesktop_zoomFactor 桥接 + state.json)。
#include <gtest/gtest.h>
#include "desktop/desktop_zoom.hpp"
#include "test_support/utils/state_file_fixture.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <vector>

namespace {

using acecode::desktop::DesktopZoom;
using acecode::desktop::clamp_desktop_zoom_factor;
using acecode::desktop::read_desktop_zoom_factor;
using acecode::desktop::write_desktop_zoom_factor;
using acecode::test_support::write_raw;

class DesktopZoomStateTest : public acecode::test_support::StateFileTest {};

nlohmann::json bridge(DesktopZoom& zoom, const std::string& request) {
    return nlohmann::json::parse(zoom.handle_bridge(request));
}

// 场景:启动时恢复上次保存的 125%,之后前端查询当前比例。
// 期望:启动即把 125% 套用到 WebView(页面加载出来就是上次的大小,不会先 100% 再跳),查询返回同一个值。
TEST(DesktopZoomTest, RestoreAppliesSavedFactorAndQueryReportsIt) {
    std::vector<double> applied;
    DesktopZoom zoom([&applied](double factor) {
        applied.push_back(factor);
        return true;
    });
    zoom.restore(1.25);

    ASSERT_EQ(applied.size(), 1u);
    EXPECT_DOUBLE_EQ(applied[0], 1.25);
    const auto result = bridge(zoom, "[]");
    EXPECT_TRUE(result["ok"].get<bool>());
    EXPECT_DOUBLE_EQ(result["factor"].get<double>(), 1.25);
    EXPECT_FALSE(zoom.changed());
}

// 场景:前端按 Ctrl+滚轮一级一级设置比例,其中一次传了越界值。
// 期望:每次都套用到 WebView 并回报实际值;越界值收进 50%–300%;与启动值不同才算需要写回。
TEST(DesktopZoomTest, SetClampsAndTracksChange) {
    std::vector<double> applied;
    DesktopZoom zoom([&applied](double factor) {
        applied.push_back(factor);
        return true;
    });
    zoom.restore(std::nullopt);

    EXPECT_DOUBLE_EQ(bridge(zoom, "[1.1]")["factor"].get<double>(), 1.1);
    EXPECT_TRUE(zoom.changed());
    EXPECT_DOUBLE_EQ(bridge(zoom, "[9]")["factor"].get<double>(), 3.0);
    EXPECT_DOUBLE_EQ(bridge(zoom, "[0.1]")["factor"].get<double>(), 0.5);
    EXPECT_DOUBLE_EQ(applied.back(), 0.5);

    // 点「重置」回到 100%,与启动值相同,退出时不必再写 state.json。
    EXPECT_DOUBLE_EQ(bridge(zoom, "[1]")["factor"].get<double>(), 1.0);
    EXPECT_FALSE(zoom.changed());
}

// 场景:前端传来坏参数(不是数组、不是数值、JSON 损坏)。
// 期望:返回 ok:false 且不改动当前比例,也不调用 WebView。
TEST(DesktopZoomTest, MalformedRequestsLeaveFactorUntouched) {
    int calls = 0;
    DesktopZoom zoom([&calls](double) {
        ++calls;
        return true;
    });
    zoom.restore(1.5);
    calls = 0;

    for (const char* request : {"{}", "[\"1.2\"]", "[null]", "not json"}) {
        const auto result = bridge(zoom, request);
        EXPECT_FALSE(result["ok"].get<bool>()) << request;
        EXPECT_DOUBLE_EQ(result["factor"].get<double>(), 1.5) << request;
    }
    EXPECT_EQ(calls, 0);
}

// 场景:WebView 套用比例失败(控制器已销毁等)。
// 期望:报 ok:false 并保留上一次成功的比例,前端据此回退显示。
TEST(DesktopZoomTest, ApplyFailureKeepsPreviousFactor) {
    bool fail = false;
    DesktopZoom zoom([&fail](double) { return !fail; });
    zoom.restore(1.0);
    fail = true;

    const auto result = bridge(zoom, "[1.25]");
    EXPECT_FALSE(result["ok"].get<bool>());
    EXPECT_DOUBLE_EQ(result["factor"].get<double>(), 1.0);
    EXPECT_DOUBLE_EQ(zoom.factor(), 1.0);
}

// 场景:平台不支持页面缩放(macOS / Linux 的 WebHost::set_zoom_factor 返回 false)。
// 期望:启动时的套用失败即判定不支持,查询也报 ok:false,前端不显示缩放浮层。
TEST(DesktopZoomTest, UnsupportedPlatformReportsUnavailable) {
    DesktopZoom zoom([](double) { return false; });
    zoom.restore(1.25);

    EXPECT_FALSE(bridge(zoom, "[]")["ok"].get<bool>());
    EXPECT_FALSE(bridge(zoom, "[1.1]")["ok"].get<bool>());
    EXPECT_DOUBLE_EQ(zoom.factor(), 1.0);
}

// 场景:非有限值进入换算(坏数据)。
// 期望:回到 100% 而不是把 NaN / 无穷大交给 WebView。
TEST(DesktopZoomTest, NonFiniteFactorFallsBackToDefault) {
    EXPECT_DOUBLE_EQ(clamp_desktop_zoom_factor(std::numeric_limits<double>::quiet_NaN()), 1.0);
    EXPECT_DOUBLE_EQ(clamp_desktop_zoom_factor(std::numeric_limits<double>::infinity()), 1.0);
}

// 场景:退出时写回缩放比例,下次启动读取;state.json 里还有别的字段。
// 期望:比例原样读回,其它字段不被覆盖。
TEST_F(DesktopZoomStateTest, RoundTripKeepsOtherStateKeys) {
    acecode::write_state_flag("some_flag", true);
    write_desktop_zoom_factor(1.75);

    const auto loaded = read_desktop_zoom_factor();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_DOUBLE_EQ(*loaded, 1.75);
    EXPECT_TRUE(acecode::read_state_flag("some_flag"));
}

// 场景:没保存过,或字段被手改成字符串 / 越界数值。
// 期望:缺失与非数值读成空(启动按 100%),越界数值收进 50%–300%。
TEST_F(DesktopZoomStateTest, MissingOrMalformedValues) {
    EXPECT_FALSE(read_desktop_zoom_factor().has_value());

    write_raw(path_, R"({"desktop_zoom_factor": "1.25"})");
    EXPECT_FALSE(read_desktop_zoom_factor().has_value());

    write_raw(path_, R"({"desktop_zoom_factor": 12})");
    ASSERT_TRUE(read_desktop_zoom_factor().has_value());
    EXPECT_DOUBLE_EQ(*read_desktop_zoom_factor(), 3.0);
}

} // namespace
