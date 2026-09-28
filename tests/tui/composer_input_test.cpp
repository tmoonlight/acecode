#include <gtest/gtest.h>
#include "tui/composer/paste.hpp"
#include "tui/composer/edit_keys.hpp"
#include "tui/composer/submit.hpp"
#include "tui/composer/clipboard_keys.hpp"
#include "tui/composer/input_component.hpp"
#include "tui/paste_handler.hpp"
#include "test_support/tui/input_fixture.hpp"
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

using acecode::tui::InputDisposition;
using acecode::tui::test_support::InputHarness;
using ftxui::Event;

// 中文回归说明：系统读取在锁外；读取期间 overlay 接管后，第二次检查必须丢弃粘贴。
TEST(ComposerInput, ClipboardReadIsUnlockedAndRechecksOverlayState) {
    InputHarness h;
    h.state.input_text = "original";
    bool unlocked = false;
    h.clipboard.on_read_text = [&] {
        unlocked = h.state.mu.try_lock();
        if (unlocked) {
            h.state.confirm_pending = true;
            h.state.mu.unlock();
        }
    };
    EXPECT_TRUE(acecode::tui::paste_clipboard_text(h.context));
    EXPECT_TRUE(unlocked);
    EXPECT_EQ(h.state.input_text, "original");
    EXPECT_EQ(h.clipboard.text_reads, 1);
}

// 中文回归说明：括号粘贴内的 Enter 只进入文本，不能误提交或触发其它输入阶段。
TEST(ComposerInput, BracketedPasteOwnsEmbeddedReturn) {
    InputHarness h;
    const Event events[] = {Event::Special("\x1b[200~"),
        Event::Character("first"), Event::Return, Event::Character("second"),
        Event::Special("\x1b[201~")};
    for (const auto& event : events)
        EXPECT_EQ(acecode::tui::handle_bracketed_paste(h.context, event), InputDisposition::Consumed);
    const auto expanded = acecode::tui::expand_placeholders(h.state.input_text, h.state.pasted_texts);
    EXPECT_EQ(expanded, "first\nsecond");
    EXPECT_TRUE(h.turn.inputs.empty());
    EXPECT_EQ(h.state.history_index, -1);
}

// 中文回归说明：长粘贴占位符按整体删除，同时移除存储内容，不能剩半个标记。
TEST(ComposerInput, BackspaceRemovesWholePastePlaceholder) {
    InputHarness h;
    const std::string full = std::string(700, 'x') + "\nsecond\nthird\nfourth";
    ASSERT_TRUE(acecode::tui::should_fold_to_placeholder(full));
    acecode::tui::insert_pasted_text_at_cursor_locked(h.state, full);
    ASSERT_EQ(h.state.pasted_texts.size(), 1u);
    EXPECT_EQ(acecode::tui::handle_composer_backspace(h.context, Event::Backspace),
        InputDisposition::Consumed);
    EXPECT_TRUE(h.state.input_text.empty());
    EXPECT_TRUE(h.state.pasted_texts.empty());
}

// 中文回归说明：首回合等待必须在 UI 锁外；提交和气泡使用展开全文，历史不保存占位符。
TEST(ComposerInput, SubmitExpandsPasteAndWaitsOutsideUiLock) {
    InputHarness h;
    const std::string full = std::string(700, 'x') + "\nsecond\nthird\nfourth";
    acecode::tui::insert_pasted_text_at_cursor_locked(h.state, full);
    bool unlocked = false;
    h.turn.before = [&] {
        unlocked = h.state.mu.try_lock();
        if (unlocked) h.state.mu.unlock();
    };
    EXPECT_EQ(acecode::tui::handle_composer_submit(h.context, Event::Return),
        InputDisposition::Consumed);
    EXPECT_TRUE(unlocked);
    EXPECT_EQ(h.turn.before_calls, 1);
    ASSERT_EQ(h.turn.inputs.size(), 1u);
    EXPECT_EQ(h.turn.inputs[0].text, full);
    EXPECT_EQ(h.state.input_history, (std::vector<std::string>{full}));
    EXPECT_EQ(h.state.conversation.back().content, full);
    EXPECT_TRUE(h.state.pasted_texts.empty());
    EXPECT_TRUE(h.state.is_waiting);
}

// 中文回归说明：忙时排队不启动第二个回合，shell 分支仍走专用 worker 入口。
TEST(ComposerInput, BusyQueueAndShellKeepSeparateDispatch) {
    InputHarness h;
    h.state.is_waiting = true;
    h.state.input_text = "next prompt";
    EXPECT_EQ(acecode::tui::handle_composer_submit(h.context, Event::Return),
        InputDisposition::Consumed);
    ASSERT_EQ(h.state.pending_queue.size(), 1u);
    EXPECT_EQ(h.state.pending_queue.front(), "next prompt");
    EXPECT_EQ(h.turn.before_calls, 0);
    EXPECT_TRUE(h.turn.inputs.empty());
    h.state.is_waiting = false;
    h.state.input_mode = acecode::InputMode::Shell;
    h.state.input_text = "echo test";
    (void)acecode::tui::handle_composer_submit(h.context, Event::Return);
    EXPECT_EQ(h.turn.shells, (std::vector<std::string>{"echo test"}));
    EXPECT_EQ(h.state.input_history.back(), "!echo test");
    EXPECT_EQ(h.state.current_thinking_phrase, "Running shell");
}

// 中文特征说明：原先 mode picker 吞 Delete，model picker 允许编辑隐藏缓冲；不可在重构中统一。
TEST(ComposerInput, DeletePreservesPickerAsymmetry) {
    InputHarness h;
    h.state.input_text = "abc";
    h.state.input_cursor = 1;
    h.state.mode_picker_open = true;
    (void)acecode::tui::handle_composer_delete(h.context, Event::Delete);
    EXPECT_EQ(h.state.input_text, "abc");
    h.state.mode_picker_open = false;
    h.state.model_picker_open = true;
    (void)acecode::tui::handle_composer_delete(h.context, Event::Delete);
    EXPECT_EQ(h.state.input_text, "ac");
}

// 中文回归说明：右键复制失败才使用 OSC52；TooLarge 不得绕过大小限制。
TEST(ComposerInput, RightClickCopyFallbackPreservesSizeGuard) {
    InputHarness h;
    h.screen.set_selection("selected");
    ftxui::Mouse mouse;
    mouse.button = ftxui::Mouse::Right;
    mouse.motion = ftxui::Mouse::Pressed;
    const auto event = Event::Mouse("right-press", mouse);
    h.clipboard.write.status = acecode::ClipboardTextWriteResult::Status::Unavailable;
    EXPECT_EQ(acecode::tui::handle_clipboard_right_click(h.context, event), InputDisposition::Consumed);
    EXPECT_EQ(h.clipboard.osc52, (std::vector<std::string>{"selected"}));
    h.clipboard.write.status = acecode::ClipboardTextWriteResult::Status::TooLarge;
    (void)acecode::tui::handle_clipboard_right_click(h.context, event);
    EXPECT_EQ(h.clipboard.osc52.size(), 1u);
    h.screen.set_selection("");
    (void)acecode::tui::handle_clipboard_right_click(h.context, event);
    EXPECT_EQ(h.clipboard.text_reads, 1);
    EXPECT_EQ(h.state.input_text, "clipboard text");
}

// 中文回归说明：替代 Renderer(bool) 后必须仍可聚焦，并在实际绘制时回填输入命中区域。
TEST(ComposerInput, ComponentRemainsFocusableAndReflectsWrappedText) {
    InputHarness h;
    h.state.input_text = "composer text";
    h.state.input_cursor = h.state.input_text.size();
    auto input = acecode::tui::make_composer_input(h.state, h.geometry.input_hit_layout);
    EXPECT_TRUE(input->Focusable());
    ftxui::Screen screen(20, 4);
    ftxui::Render(screen, input->Render());
    EXPECT_EQ(h.geometry.input_hit_layout.input_value, "composer text");
    EXPECT_FALSE(h.geometry.input_hit_layout.regions.empty());
}
