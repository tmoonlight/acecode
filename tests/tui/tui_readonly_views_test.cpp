#include <gtest/gtest.h>
#include "tui/render/header_view.hpp"
#include "tui/render/picker_views.hpp"
#include "tui/render/activity_indicator_view.hpp"
#include "tui/render/prompt_status_view.hpp"
#include "tui/render/link_hover_tooltip.hpp"
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

namespace {
std::string snapshot(const ftxui::Element& element, int width = 120, int height = 12) {
    ftxui::Screen screen(width, height);
    ftxui::Render(screen, element);
    std::string value;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) value += screen.PixelAt(x, y).character;
        value += '\n';
    }
    return value;
}
}

// 中文回归说明：三种根布局决定横幅内容；侧栏存在且已有会话时不再重复横幅。
TEST(TuiReadonlyViews, HeaderPreservesLayoutBranches) {
    acecode::TuiState state;
    state.status_line = "ready";
    state.update_notice = "update available";
    const auto compat = snapshot(acecode::tui::render_header_view(
        state, "acecode-v-test", "C:/workspace", true, false, false));
    EXPECT_NE(compat.find("acecode-v-test"), std::string::npos);
    EXPECT_NE(compat.find("ready"), std::string::npos);
    EXPECT_NE(compat.find("C:/workspace"), std::string::npos);
    const auto narrow = snapshot(acecode::tui::render_header_view(
        state, "acecode-v-test", "C:/workspace", false, false, false));
    EXPECT_NE(narrow.find("update available"), std::string::npos);
    const auto sidebar = snapshot(acecode::tui::render_header_view(
        state, "acecode-v-test", "C:/workspace", false, true, true));
    EXPECT_EQ(sidebar.find_first_not_of(" \n"), std::string::npos);
}

// 中文回归说明：气泡按显示格截断，右上空间不足时翻转，极窄终端不生成布局。
TEST(TuiReadonlyViews, TooltipPlacementTable) {
    struct Row { int width, height, mx, my, x, y, bubble_width; };
    const Row rows[] = {
        {80, 24, 10, 10, 12, 6, 5}, {80, 24, 79, 10, 72, 6, 5},
        {80, 24, 0, 0, 2, 1, 5}, {80, 3, 78, 2, 71, 0, 5},
        {4, 3, 3, 0, 0, 0, 4},
    };
    acecode::TuiState state;
    state.hover_link_href = "abc";
    for (const auto& row : rows) {
        state.hover_link_x = row.mx;
        state.hover_link_y = row.my;
        const auto tip = acecode::tui::place_link_hover_tooltip(state, row.width, row.height);
        ASSERT_TRUE(tip);
        EXPECT_EQ(tip->x, row.x);
        EXPECT_EQ(tip->y, row.y);
        EXPECT_EQ(tip->width, row.bubble_width);
        EXPECT_LE(tip->x + tip->width, row.width);
        EXPECT_LE(tip->y + tip->height, row.height);
    }
    EXPECT_FALSE(acecode::tui::place_link_hover_tooltip(state, 3, 24));
    EXPECT_FALSE(acecode::tui::place_link_hover_tooltip(state, 80, 2));
}

// 中文回归说明：ask/confirm 不得渲染普通编辑器，否则会残留可点击的旧输入区域。
TEST(TuiReadonlyViews, OverlayNeverRendersComposerAndClearsItsHitRegions) {
    acecode::TuiState state;
    acecode::tui::AskQuestionFrame ask;
    acecode::tui::InputTextHitLayout hit;
    acecode::PermissionManager permissions;
    int composer_calls = 0;
    const auto composer = [&] {
        ++composer_calls;
        hit.input_value = "composer-text";
        return ftxui::text("composer-text");
    };
    for (bool is_ask : {true, false}) {
        state.ask_pending = is_ask;
        state.confirm_pending = !is_ask;
        state.confirm_tool_name = "test-tool";
        hit.box = {1, 2, 3, 4};
        hit.regions.push_back({});
        hit.input_value = "stale";
        auto view = acecode::tui::render_prompt_status_view(
            state, ask, hit, permissions, 120, false, false, false, composer);
        const auto text = snapshot(view.prompt);
        EXPECT_EQ(composer_calls, 0);
        EXPECT_TRUE(hit.box.IsEmpty());
        EXPECT_TRUE(hit.regions.empty());
        EXPECT_TRUE(hit.input_value.empty());
        EXPECT_EQ(text.find("composer-text"), std::string::npos);
        EXPECT_NE(text.find(is_ask ? "answering" : "awaiting confirmation"), std::string::npos);
    }
    state.ask_pending = state.confirm_pending = false;
    auto view = acecode::tui::render_prompt_status_view(
        state, ask, hit, permissions, 120, false, false, false, composer);
    EXPECT_EQ(composer_calls, 1);
    EXPECT_NE(snapshot(view.prompt).find("composer-text"), std::string::npos);
    EXPECT_EQ(hit.input_value, "composer-text");
}

// 中文回归说明：未打开的 picker 和 conhost 的动画区保持空白，不占用编辑空间。
TEST(TuiReadonlyViews, InactiveViewsRemainEmpty) {
    acecode::TuiState state;
    auto pickers = acecode::tui::render_picker_views(state);
    for (const auto& view : {pickers.resume, pickers.rewind, pickers.model, pickers.mode})
        EXPECT_EQ(snapshot(view).find_first_not_of(" \n"), std::string::npos);
    state.is_waiting = true;
    auto activity = acecode::tui::render_activity_indicator_view(state, true, false, 0);
    EXPECT_EQ(snapshot(activity.thinking).find_first_not_of(" \n"), std::string::npos);
}
