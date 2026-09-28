#include <gtest/gtest.h>
#include "tui/app/tui_event_router.hpp"
#include "tui/composer/input_component.hpp"
#include "test_support/tui/input_fixture.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include "tui/input/event_prelude.hpp"
#include "tui/input/control_keys.hpp"
#include "tui/input/composer_pointer.hpp"
#include "tui/input/chat_keys.hpp"
#include "tui/input/mouse_router.hpp"
#include "tui/composer/paste.hpp"
#include "tui/composer/clipboard_keys.hpp"
#include "tui/composer/pending_attachment.hpp"
#include "tui/composer/submit.hpp"
#include "tui/composer/edit_keys.hpp"
#include "tui/overlays/ask_question_input.hpp"
#include "tui/overlays/confirm_overlay_input.hpp"
#include "tui/overlays/rewind_picker_input.hpp"
#include "tui/overlays/completion_dropdown_input.hpp"
#include "tui/overlays/list_picker_input.hpp"

namespace {
using acecode::tui::TuiEventRouter;
using acecode::tui::test_support::InputHarness;
using ftxui::Event;
void set_overlay(acecode::TuiState& state, int mode) {
    state.resume_picker_active = mode == 1;
    state.model_picker_open = mode == 2;
    state.mode_picker_open = mode == 3;
    state.rewind_picker_active = mode == 4;
    state.confirm_pending = mode == 5;
    state.confirm_tool_name = "file_write";
}
}
// 中文结构契约：逐键比较函数地址和原行号，避免按模块合并后改变交错顺序。
TEST(TuiEventRouter, RouteOrderRetainsIndividualKeysAndOriginalLocations) {
    using acecode::tui::InputRoute;
    const InputRoute expected[] = {
        {"prelude", 6846, acecode::tui::handle_event_prelude},
        {"bracketed_paste", 6915, acecode::tui::handle_bracketed_paste},
        {"ctrl_v", 6946, acecode::tui::handle_clipboard_ctrl_v},
        {"alt_v", 6949, acecode::tui::handle_clipboard_alt_v},
        {"ctrl_c", 6952, acecode::tui::handle_ctrl_c},
        {"pending_attachment", 7000, acecode::tui::handle_pending_attachment_input},
        {"composer_pointer", 7004, acecode::tui::handle_composer_pointer},
        {"ask_guard", 7041, acecode::tui::handle_ask_question_input},
        {"remote_confirm", 7064, acecode::tui::pump_remote_confirm},
        {"confirm", 7080, acecode::tui::handle_confirm_overlay_input},
        {"rewind", 7091, acecode::tui::handle_rewind_picker_input},
        {"path_reference", 7095, acecode::tui::handle_path_reference_input},
        {"slash", 7100, acecode::tui::handle_slash_dropdown_input},
        {"enter", 7105, acecode::tui::handle_composer_submit},
        {"picker_page", 7343, acecode::tui::handle_list_picker_page},
        {"chat_page_up", 7400, acecode::tui::handle_chat_page_up},
        {"chat_page_down", 7436, acecode::tui::handle_chat_page_down},
        {"chat_alt_up", 7477, acecode::tui::handle_chat_alt_up},
        {"chat_alt_down", 7489, acecode::tui::handle_chat_alt_down},
        {"chat_home", 7501, acecode::tui::handle_chat_home},
        {"chat_end", 7512, acecode::tui::handle_chat_end},
        {"escape", 7521, acecode::tui::handle_escape},
        {"tab", 7607, acecode::tui::handle_tab},
        {"shift_tab", 7612, acecode::tui::handle_shift_tab},
        {"mouse", 7636, acecode::tui::handle_mouse},
        {"shift_arrow", 8196, acecode::tui::handle_composer_shift_arrow},
        {"up", 8255, acecode::tui::handle_composer_up},
        {"down", 8288, acecode::tui::handle_composer_down},
        {"left", 8328, acecode::tui::handle_composer_left},
        {"right", 8353, acecode::tui::handle_composer_right},
        {"ctrl_a", 8394, acecode::tui::handle_composer_ctrl_a},
        {"input_home", 8410, acecode::tui::handle_composer_home},
        {"ctrl_o", 8421, acecode::tui::handle_chat_ctrl_o},
        {"ctrl_e", 8433, acecode::tui::handle_chat_ctrl_e},
        {"input_end", 8464, acecode::tui::handle_composer_end},
        {"delete", 8473, acecode::tui::handle_composer_delete},
        {"backspace", 8509, acecode::tui::handle_composer_backspace},
        {"character", 8557, acecode::tui::handle_composer_character},
    };
    const auto& actual = TuiEventRouter::routes();
    ASSERT_EQ(actual.size(), std::size(expected));
    for (std::size_t i = 0; i < actual.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_STREQ(actual[i].name, expected[i].name);
        EXPECT_EQ(actual[i].handler, expected[i].handler);
        EXPECT_EQ(actual[i].original_line, expected[i].original_line);
    }
}

// 中文特征矩阵：浮层只拦截原有按键，其余按原顺序走聊天和编辑入口。
TEST(TuiEventRouter, FullKeyMatrixAcrossSixOverlayStates) {
    struct Key { const char* name; Event event; enum Kind { Always, Escape, Tab, Fallback } kind = Always; };
    const Key keys[] = {
        {"enter", Event::Return}, {"up", Event::ArrowUp}, {"down", Event::ArrowDown},
        {"left", Event::ArrowLeft}, {"right", Event::ArrowRight},
        {"home", Event::Home}, {"end", Event::End},
        {"page-up", Event::PageUp}, {"page-down", Event::PageDown},
        {"alt-up", Event::Special("\x1b[1;3A")}, {"alt-down", Event::Special("\x1b[1;3B")},
        {"escape", Event::Escape, Key::Escape}, {"tab", Event::Tab, Key::Tab},
        {"backspace", Event::Backspace}, {"delete", Event::Delete},
        {"ctrl-e", Event::Special("\x05")}, {"ctrl-o", Event::Special("\x0f")},
        {"ctrl-a", Event::Special("\x01")}, {"ctrl-c", Event::Special("\x03")},
        {"ctrl-v", Event::Special("\x16")}, {"alt-v", Event::Special("\x1bv")},
        {"ctrl-p", Event::Special("\x10"), Key::Fallback},
        {"ctrl-n", Event::Special("\x0e"), Key::Fallback},
        {"shift-tab", Event::TabReverse},
        {"character", Event::Character('x')}, {"digit", Event::Character('1')},
        {"space", Event::Character(' ')},
        {"custom", Event::Custom, Key::Fallback},
        {"unknown", Event::Special("unknown-key"), Key::Fallback},
        {"cursor-position", Event::CursorPosition("position", 2, 3), Key::Fallback},
        {"cursor-shape", Event::CursorShape("shape", 2), Key::Fallback},
    };
    for (int mode = 0; mode < 6; ++mode) {
        for (const auto& key : keys) {
            SCOPED_TRACE(std::to_string(mode) + ":" + key.name);
            InputHarness h;
            set_overlay(h.state, mode);
            TuiEventRouter router(h.context);
            const bool expected = mode == 4 || key.kind == Key::Always
                || (key.kind == Key::Escape && mode != 0)
                || (key.kind == Key::Tab && mode == 3);
            EXPECT_EQ(router.handle(key.event), expected);
        }
    }
}

// 中文回归说明：确认/列表不遮蔽 Ctrl+E，rewind 独占输入时不修改聊天展开状态。
TEST(TuiEventRouter, CtrlEReachesChatThroughListsAndConfirm) {
    for (int mode = 0; mode < 6; ++mode) {
        InputHarness h;
        set_overlay(h.state, mode);
        acecode::TuiState::Message msg;
        msg.role = "tool_result";
        msg.summary = acecode::ToolSummary{};
        h.state.conversation.push_back(msg);
        h.state.chat_focus_index = 0;
        TuiEventRouter router(h.context);
        EXPECT_TRUE(router.handle(Event::Special("\x05")));
        EXPECT_EQ(h.state.conversation[0].expanded, mode != 4);
    }
}

// 中文回归说明：ask 的 Declined 必须停止真实路由，不能推进后面的远程确认泵。
TEST(TuiEventRouter, AskCustomStopsBeforeRemoteConfirmation) {
    InputHarness h;
    h.state.ask_pending = true;
    h.state.ask_session = std::make_shared<acecode::tui::AskQuestionSession>(
        std::vector<acecode::AskQuestion>{});
    h.state.remote_confirm_queue.push_back({"child", "request", "file_write", "args", "child"});
    TuiEventRouter router(h.context);
    EXPECT_FALSE(router.handle(Event::Custom));
    EXPECT_FALSE(h.state.confirm_pending);
    EXPECT_EQ(h.state.remote_confirm_queue.size(), 1u);
}

// 中文回归说明：定位 composer 光标后返回 false 给 FTXUI，同一次按下不得再启动聊天拖选。
TEST(TuiEventRouter, ComposerPointerDeclinesAfterCursorPlacement) {
    InputHarness h;
    h.state.input_text = "abc";
    h.state.input_cursor = 0;
    h.geometry.input_hit_layout.input_value = "abc";
    h.geometry.input_hit_layout.box = {10, 12, 5, 5};
    h.geometry.input_hit_layout.regions.push_back({{10, 12, 5, 5}, 0, 3});
    h.state.remote_confirm_queue.push_back({"child", "request", "file_write", "args", "child"});
    ftxui::Mouse mouse;
    mouse.button = ftxui::Mouse::Left;
    mouse.motion = ftxui::Mouse::Pressed;
    mouse.x = 11; mouse.y = 5;
    TuiEventRouter router(h.context);
    EXPECT_FALSE(router.handle(Event::Mouse("left-press", mouse)));
    EXPECT_GT(h.state.input_cursor, 0u);
    EXPECT_FALSE(h.state.drag_left_pressed);
    EXPECT_EQ(h.state.remote_confirm_queue.size(), 1u);
}

// 中文回归说明：忙时 Ctrl+C 仅发 Escape，再次经过路由时才取消 turn。
TEST(TuiEventRouter, BusyCtrlCReentersRouterAsEscape) {
    InputHarness h;
    h.state.is_waiting = true;
    TuiEventRouter router(h.context);
    EXPECT_TRUE(router.handle(Event::Special("\x03")));
    EXPECT_EQ(h.turn.cancellations, 0);
    auto events = h.screen.take_events();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0], Event::Escape);
    EXPECT_TRUE(router.handle(events[0]));
    EXPECT_EQ(h.turn.cancellations, 1);
    EXPECT_TRUE(h.state.turn_interrupted_by_user);
}

// 中文回归说明：右键复制必须发生在鼠标 Pressed 清理悬停之后，不能前移成顶层快捷键。
TEST(TuiEventRouter, RightCopyClearsHoverBeforeClipboardWrite) {
    InputHarness h;
    h.screen.set_selection("selection");
    h.state.hover_link_href = "https://example.test/";
    h.state.hover_link_visible = true;
    h.clipboard.on_write_text = [&] {
        EXPECT_TRUE(h.state.hover_link_href.empty());
        EXPECT_FALSE(h.state.hover_link_visible);
    };
    ftxui::Mouse mouse;
    mouse.button = ftxui::Mouse::Right;
    mouse.motion = ftxui::Mouse::Pressed;
    TuiEventRouter router(h.context);
    EXPECT_TRUE(router.handle(Event::Mouse("right-press", mouse)));
    EXPECT_EQ(h.clipboard.written, (std::vector<std::string>{"selection"}));
}

// 中文回归说明：设置/管理激活时不调用聊天 router，确认泵到关闭界面后才运行。
TEST(TuiEventRouter, FullScreenSurfaceOwnsInputUntilChatIsSelected) {
    InputHarness h;
    TuiEventRouter router(h.context);
    auto composer = acecode::tui::make_composer_input(h.state, h.geometry.input_hit_layout);
    auto chat = router.wrap(composer);
    int active_surface = 1, surface_events = 0;
    auto surface = ftxui::CatchEvent(ftxui::Renderer([] { return ftxui::text("settings"); }),
        [&](Event) { ++surface_events; return true; });
    auto root = ftxui::Container::Tab({chat, surface}, &active_surface);
    h.state.remote_confirm_queue.push_back({"child", "request", "file_write", "args", "child"});
    EXPECT_TRUE(root->OnEvent(Event::Custom));
    EXPECT_EQ(surface_events, 1);
    EXPECT_FALSE(h.state.confirm_pending);
    EXPECT_EQ(h.state.remote_confirm_queue.size(), 1u);
    active_surface = 0;
    (void)root->OnEvent(Event::Custom);
    EXPECT_TRUE(h.state.confirm_pending);
    EXPECT_TRUE(h.state.remote_confirm_queue.empty());
}

// 中文生命周期说明：FTXUI 组件晚于 router 释放时，撤销后的闭包不再访问输入上下文。
TEST(TuiEventRouter, WrappedComponentCanOutliveRouter) {
    InputHarness h;
    ftxui::Component component;
    {
        TuiEventRouter router(h.context);
        component = router.wrap(acecode::tui::make_composer_input(h.state, h.geometry.input_hit_layout));
    }
    EXPECT_FALSE(component->OnEvent(Event::Character('x')));
    EXPECT_TRUE(h.state.input_text.empty());
}
