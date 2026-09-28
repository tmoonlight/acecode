#include <gtest/gtest.h>
#include "tui/overlays/ask_question_input.hpp"
#include "tui/overlays/confirm_overlay_input.hpp"
#include "tui/overlays/rewind_picker_input.hpp"
#include "tui/overlays/list_picker_input.hpp"
#include "tui/input/chat_keys.hpp"
#include "test_support/tui/fake_screen_port.hpp"
#include <array>

namespace {
using acecode::tui::InputDisposition;
using ftxui::Event;

InputDisposition picker_stages(acecode::TuiState& state,
    acecode::tui::IScreenPort& screen, acecode::tui::ChatViewport& viewport, Event event) {
    auto disposition = acecode::tui::handle_confirm_overlay_input(state, screen, event);
    if (disposition != InputDisposition::Continue) return disposition;
    disposition = acecode::tui::handle_rewind_picker_input(state, screen, event, viewport);
    if (disposition != InputDisposition::Continue) return disposition;
    disposition = acecode::tui::list_picker_page(state, screen, event);
    if (disposition != InputDisposition::Continue) return disposition;
    std::lock_guard<std::mutex> lock(state.mu);
    if (event == Event::Return)
        return acecode::tui::list_picker_enter_locked(state, screen, viewport);
    if (event == Event::Escape)
        return acecode::tui::list_picker_escape_locked(state, screen, viewport);
    if (event == Event::ArrowUp)
        return acecode::tui::list_picker_up_locked(state, screen);
    if (event == Event::ArrowDown)
        return acecode::tui::list_picker_down_locked(state, screen);
    if (event.is_character())
        return acecode::tui::list_picker_character_locked(state, screen, viewport, event);
    return InputDisposition::Continue;
}
}

// 中文特征说明：先记录原 handler 的吞键边界，再抽取实现；统一验收时运行。
// 此矩阵只覆盖浮层阶段，Continue 仍交给后面的聊天/编辑阶段；不能一律改成吞键。
TEST(OverlayInputCharacterization, KeyMatrixAcrossSixOverlayStates) {
    struct KeyRow { const char* name; Event event; bool list_consumes, confirm_consumes; };
    const KeyRow keys[] = {
        {"enter", Event::Return, true, true},
        {"up", Event::ArrowUp, true, true}, {"down", Event::ArrowDown, true, true},
        {"left", Event::ArrowLeft, false, false}, {"right", Event::ArrowRight, false, false},
        {"home", Event::Home, true, false}, {"end", Event::End, true, false},
        {"page-up", Event::PageUp, true, false}, {"page-down", Event::PageDown, true, false},
        {"escape", Event::Escape, true, true}, {"tab", Event::Tab, false, false},
        {"backspace", Event::Backspace, false, false}, {"delete", Event::Delete, false, false},
        {"custom", Event::Custom, false, false},
        {"ctrl-e", Event::Special("\x05"), false, false},
        {"ctrl-o", Event::Special("\x0f"), false, false},
        {"ctrl-a", Event::Special("\x01"), false, false},
        {"ctrl-c", Event::Special("\x03"), false, false},
        {"ctrl-v", Event::Special("\x16"), false, false},
        {"ctrl-p", Event::Special("\x10"), false, false},
        {"ctrl-n", Event::Special("\x0e"), false, false},
        {"shift-tab", Event::TabReverse, false, true},
        {"character", Event::Character('x'), true, true},
        {"digit", Event::Character('1'), true, true},
        {"space", Event::Character(' '), true, true},
        {"unknown", Event::Special("unknown-key"), false, false},
    };
    for (int mode = 0; mode < 6; ++mode) {
        for (const auto& key : keys) {
            SCOPED_TRACE(std::to_string(mode) + ":" + key.name);
            acecode::TuiState state;
            state.resume_picker_active = mode == 1;
            state.model_picker_open = mode == 2;
            state.mode_picker_open = mode == 3;
            state.rewind_picker_active = mode == 4;
            state.confirm_pending = mode == 5;
            state.confirm_tool_name = "file_write";
            acecode::tui::test_support::FakeScreenPort screen;
            acecode::tui::ChatViewport viewport;
            const bool consumed = mode == 4 || (mode >= 1 && mode <= 3 && key.list_consumes)
                || (mode == 5 && key.confirm_consumes);
            EXPECT_EQ(picker_stages(state, screen, viewport, key.event),
                consumed ? InputDisposition::Consumed : InputDisposition::Continue);
        }
    }
}

// 中文特征说明：列表 picker 和确认框不遮蔽 Ctrl+E；rewind 的全吞键行为原样保留。
TEST(OverlayInputCharacterization, CtrlEStillExpandsFocusedResultWithListPickerOpen) {
    for (int mode = 0; mode < 6; ++mode) {
        acecode::TuiState state;
        state.resume_picker_active = mode == 1;
        state.model_picker_open = mode == 2;
        state.mode_picker_open = mode == 3;
        state.rewind_picker_active = mode == 4;
        state.confirm_pending = mode == 5;
        acecode::TuiState::Message msg;
        msg.role = "tool_result";
        msg.summary = acecode::ToolSummary{};
        state.conversation.push_back(msg);
        state.chat_focus_index = 0;
        acecode::tui::test_support::FakeScreenPort screen;
        acecode::tui::ChatViewport viewport;
        auto event = Event::Special("\x05");
        auto disposition = picker_stages(state, screen, viewport, event);
        if (disposition == InputDisposition::Continue)
            disposition = acecode::tui::handle_chat_ctrl_e(state, screen, viewport, event);
        EXPECT_EQ(disposition, InputDisposition::Consumed);
        EXPECT_EQ(state.conversation[0].expanded, mode != 4);
    }
}

// 中文回归说明：ask 的 Custom 返回 Declined，路由必须立即停止并把 false 交回 FTXUI。
TEST(OverlayInputCharacterization, AskCustomDeclinesWithoutTouchingNextHandler) {
    acecode::TuiState state;
    state.ask_pending = true;
    state.ask_session = std::make_shared<acecode::tui::AskQuestionSession>(
        std::vector<acecode::AskQuestion>{});
    acecode::tui::test_support::FakeScreenPort screen;
    acecode::tui::AskQuestionFrame frame;
    auto event = Event::Custom;
    int subsequent_calls = 0;
    auto disposition = acecode::tui::handle_ask_question_input(state, screen, event, frame);
    if (disposition == InputDisposition::Continue) ++subsequent_calls;
    EXPECT_EQ(disposition, InputDisposition::Declined);
    EXPECT_EQ(subsequent_calls, 0);
    ASSERT_TRUE(acecode::tui::input_result(disposition).has_value());
    EXPECT_FALSE(*acecode::tui::input_result(disposition));
    EXPECT_TRUE(screen.take_events().empty());
}
