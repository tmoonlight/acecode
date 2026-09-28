#include "tui/input/event_prelude.hpp"
#include "tui/terminal_key_event.hpp"
#include "tui/input/input_trace.hpp"
#include "utils/logger.hpp"
#include "tui/model/input_state.hpp"
#include "tui/redraw_pacer.hpp"

using ftxui::Event;
using ftxui::Mouse;
using ftxui::Box;

namespace acecode::tui {
InputDisposition handle_event_prelude(TuiInputContext& context, const Event& event) {
    auto& state = context.state;
    auto& screen = context.screen;
    auto& chat_box = context.viewport.chat_box;
    auto& scrollbar_box = context.geometry.scrollbar_box;
    auto& ask_question_frame = context.geometry.ask_question_frame;
    if (event != Event::Custom &&
        !event.is_mouse() &&
        !event.is_cursor_position() &&
        !event.is_cursor_shape()) {
        context.last_keyboard_input_at_ms.store(
            tui::monotonic_milliseconds(), std::memory_order_release);
    }
    ACECODE_INPUT_TRACE(
    if (event != Event::Custom &&
        !event.is_cursor_position() &&
        !event.is_cursor_shape()) {
        std::string state_snapshot;
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state_snapshot =
                " ask=" + std::string(state.ask_pending ? "1" : "0") +
                " confirm=" + std::string(state.confirm_pending ? "1" : "0") +
                " ctrl_c_armed=" + std::string(state.ctrl_c_armed ? "1" : "0") +
                " resume=" + std::string(state.resume_picker_active ? "1" : "0") +
                " model=" + std::string(state.model_picker_open ? "1" : "0") +
                " mode=" + std::string(state.mode_picker_open ? "1" : "0") +
                " waiting=" + std::string(state.is_waiting ? "1" : "0") +
                " tool=" + std::string(state.tool_running ? "1" : "0") +
                " focus=" + std::to_string(state.chat_focus_index) +
                " line_offset=" + std::to_string(state.chat_line_offset) +
                " follow_tail=" + std::string(state.chat_follow_tail ? "1" : "0");
        }
        LOG_DEBUG("[input] received " + tui::event_for_log(event) +
                  " chat_box=" + tui::box_for_log(chat_box) +
                  " scrollbar_box=" + tui::box_for_log(scrollbar_box) +
                  " ask_scrollbar_box=" + tui::box_for_log(ask_question_frame.scrollbar_box) +
                  " ask_overlay_box=" + tui::box_for_log(ask_question_frame.overlay_box) +
                  state_snapshot);
    }
    );

    // drag-autoscroll: 事件线程入口处消费 anim_thread 攒下的 selection 偏移
    // 补偿. 所有对 FTXUI selection_data_ 的写都发生在这条路径上, 跟
    // HandleSelection 串行 — 避免跨线程数据竞争. 不 return, 让事件继续正常处理.
    {
        int dy = 0;
        {
            std::lock_guard<std::mutex> lk(state.mu);
            dy = state.pending_shift_dy;
            state.pending_shift_dy = 0;
        }
        if (dy != 0) {
            ACECODE_INPUT_TRACE(
            LOG_DEBUG("[drag-select] consuming pending ShiftSelection dy=" +
                      std::to_string(dy));
            );
            screen.shift_selection(0, dy);
        }
    }
    const bool ctrl_c_event =
        tui::matches_terminal_codepoint(event, 'c', tui::kTerminalCtrl);
    if (!ctrl_c_event &&
        event != Event::Custom &&
        !event.is_mouse() &&
        !event.is_cursor_position() &&
        !event.is_cursor_shape()) {
        std::lock_guard<std::mutex> lk(state.mu);
        tui::cancel_ctrl_c_exit_locked(state);
    }

    return InputDisposition::Continue;
}

}
