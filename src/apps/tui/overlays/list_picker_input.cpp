#include "tui/input/tui_input_context.hpp"
#include "tui/overlays/list_picker_input.hpp"
#include "tui/terminal_key_event.hpp"
#include "tui/picker_scroll.hpp"
#include <algorithm>
using ftxui::Event;
using ftxui::Mouse;
using ftxui::Box;

namespace acecode::tui {
InputDisposition list_picker_enter_locked(TuiState& state, IScreenPort& screen, ChatViewport& viewport) {
    // Handle resume picker: Enter confirms selection
    if (state.resume_picker_active) {
        if (state.resume_selected >= 0 &&
            state.resume_selected < static_cast<int>(state.resume_items.size())) {
            auto sid = state.resume_items[state.resume_selected].id;
            auto cb = state.resume_callback;
            state.resume_picker_active = false;
            state.resume_items.clear();
            state.resume_view_offset = 0;
            state.resume_callback = nullptr;
            state.input_text.clear(); state.pasted_texts.clear();
            state.input_cursor = 0;
            state.clear_input_selection();
            const size_t before_resume_messages =
                state.conversation.size();
            if (cb) cb(sid);
            if (state.conversation.size() != before_resume_messages) {
                viewport.reset(state);
            }
            viewport.clamp_focus(state);
        }
        screen.post_event(Event::Custom);
        return InputDisposition::Consumed;
    }

    // /model picker: Enter confirms,callback 在持 state.mu 的同
    // 一锁内跑(和 /resume 同款约定),完成后清状态 + post 一帧。
    if (state.model_picker_open) {
        if (state.model_picker_selected >= 0 &&
            state.model_picker_selected <
                static_cast<int>(state.model_picker_options.size())) {
            auto name = state.model_picker_options[state.model_picker_selected].name;
            auto cb = state.model_picker_callback;
            state.model_picker_open = false;
            state.model_picker_options.clear();
            state.model_picker_selected = 0;
            state.model_picker_view_offset = 0;
            state.model_picker_callback = nullptr;
            if (cb) cb(name);
            viewport.clamp_focus(state);
        }
        screen.post_event(Event::Custom);
        return InputDisposition::Consumed;
    }

    if (state.mode_picker_open) {
        if (state.mode_picker_selected >= 0 &&
            state.mode_picker_selected <
                static_cast<int>(state.mode_picker_options.size())) {
            const PermissionMode mode =
                state.mode_picker_options[state.mode_picker_selected].mode;
            auto callback = state.mode_picker_callback;
            state.mode_picker_open = false;
            state.mode_picker_options.clear();
            state.mode_picker_selected = 0;
            state.mode_picker_callback = nullptr;
            if (callback) callback(mode);
            viewport.clamp_focus(state);
        }
        screen.post_event(Event::Custom);
        return InputDisposition::Consumed;
    }

    return InputDisposition::Continue;
}

InputDisposition list_picker_page(TuiState& state, IScreenPort& screen, const ftxui::Event& event) {
    if (event == Event::PageUp || event == Event::PageDown ||
        event == Event::Home || event == Event::End) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.resume_picker_active) {
            const int total = static_cast<int>(state.resume_items.size());
            if (total > 0) {
                const int step = acecode::tui::kResumePickerVisibleRows;
                if (event == Event::PageUp) {
                    state.resume_selected = std::max(0, state.resume_selected - step);
                } else if (event == Event::PageDown) {
                    state.resume_selected = std::min(total - 1, state.resume_selected + step);
                } else if (event == Event::Home) {
                    state.resume_selected = 0;
                } else {  // End
                    state.resume_selected = total - 1;
                }
                state.resume_view_offset = acecode::tui::scroll_to_keep_visible(
                    state.resume_selected, state.resume_view_offset, step, total);
                screen.post_event(Event::Custom);
            }
            return InputDisposition::Consumed;
        }
        if (state.model_picker_open) {
            const int total = static_cast<int>(state.model_picker_options.size());
            if (total > 0) {
                const int step = acecode::tui::kResumePickerVisibleRows;
                if (event == Event::PageUp) {
                    state.model_picker_selected =
                        std::max(0, state.model_picker_selected - step);
                } else if (event == Event::PageDown) {
                    state.model_picker_selected =
                        std::min(total - 1, state.model_picker_selected + step);
                } else if (event == Event::Home) {
                    state.model_picker_selected = 0;
                } else {  // End
                    state.model_picker_selected = total - 1;
                }
                state.model_picker_view_offset = acecode::tui::scroll_to_keep_visible(
                    state.model_picker_selected, state.model_picker_view_offset,
                    step, total);
                screen.post_event(Event::Custom);
            }
            return InputDisposition::Consumed;
        }
        if (state.mode_picker_open) {
            const int total = static_cast<int>(state.mode_picker_options.size());
            if (total > 0) {
                if (event == Event::PageUp || event == Event::Home) {
                    state.mode_picker_selected = 0;
                } else {
                    state.mode_picker_selected = total - 1;
                }
                screen.post_event(Event::Custom);
            }
            return InputDisposition::Consumed;
        }
    }
    return InputDisposition::Continue;
}

InputDisposition list_picker_escape_locked(TuiState& state, IScreenPort& screen, ChatViewport& viewport) {
    // Escape during resume picker → cancel
    if (state.resume_picker_active) {
        state.resume_picker_active = false;
        state.resume_items.clear();
        state.resume_view_offset = 0;
        state.resume_callback = nullptr;
        state.input_text.clear(); state.pasted_texts.clear();
        state.input_cursor = 0;
        state.clear_input_selection();
        state.conversation.push_back({"system", "Resume cancelled.", false});
        state.chat_follow_tail = true;
        viewport.clamp_focus(state);
        screen.post_event(Event::Custom);
        return InputDisposition::Consumed;
    }
    // Escape during /model picker → cancel(不写气泡 —— 保持安静,
    // 用户大概率只是看了一眼当前模型就关掉)。
    if (state.model_picker_open) {
        state.model_picker_open = false;
        state.model_picker_options.clear();
        state.model_picker_selected = 0;
        state.model_picker_view_offset = 0;
        state.model_picker_callback = nullptr;
        viewport.clamp_focus(state);
        screen.post_event(Event::Custom);
        return InputDisposition::Consumed;
    }
    if (state.mode_picker_open) {
        state.mode_picker_open = false;
        state.mode_picker_options.clear();
        state.mode_picker_selected = 0;
        state.mode_picker_callback = nullptr;
        viewport.clamp_focus(state);
        screen.post_event(Event::Custom);
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

InputDisposition list_picker_up_locked(TuiState& state, IScreenPort& screen) {
    if (state.resume_picker_active) {
        if (state.resume_selected > 0) state.resume_selected--;
        state.resume_view_offset = acecode::tui::scroll_to_keep_visible(
            state.resume_selected, state.resume_view_offset,
            acecode::tui::kResumePickerVisibleRows,
            static_cast<int>(state.resume_items.size()));
        screen.post_event(Event::Custom);
        return InputDisposition::Consumed;
    }
    if (state.model_picker_open) {
        if (state.model_picker_selected > 0) state.model_picker_selected--;
        state.model_picker_view_offset = acecode::tui::scroll_to_keep_visible(
            state.model_picker_selected, state.model_picker_view_offset,
            acecode::tui::kResumePickerVisibleRows,
            static_cast<int>(state.model_picker_options.size()));
        screen.post_event(Event::Custom);
        return InputDisposition::Consumed;
    }
    if (state.mode_picker_open) {
        if (state.mode_picker_selected > 0) state.mode_picker_selected--;
        screen.post_event(Event::Custom);
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

InputDisposition list_picker_down_locked(TuiState& state, IScreenPort& screen) {
    if (state.resume_picker_active) {
        if (state.resume_selected < static_cast<int>(state.resume_items.size()) - 1)
            state.resume_selected++;
        state.resume_view_offset = acecode::tui::scroll_to_keep_visible(
            state.resume_selected, state.resume_view_offset,
            acecode::tui::kResumePickerVisibleRows,
            static_cast<int>(state.resume_items.size()));
        screen.post_event(Event::Custom);
        return InputDisposition::Consumed;
    }
    if (state.model_picker_open) {
        if (state.model_picker_selected <
            static_cast<int>(state.model_picker_options.size()) - 1)
            state.model_picker_selected++;
        state.model_picker_view_offset = acecode::tui::scroll_to_keep_visible(
            state.model_picker_selected, state.model_picker_view_offset,
            acecode::tui::kResumePickerVisibleRows,
            static_cast<int>(state.model_picker_options.size()));
        screen.post_event(Event::Custom);
        return InputDisposition::Consumed;
    }
    if (state.mode_picker_open) {
        if (state.mode_picker_selected <
            static_cast<int>(state.mode_picker_options.size()) - 1) {
            state.mode_picker_selected++;
        }
        screen.post_event(Event::Custom);
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

InputDisposition list_picker_character_locked(TuiState& state, IScreenPort& screen, ChatViewport& viewport, const ftxui::Event& event) {
    // During resume picker, digit keys select directly
    if (state.resume_picker_active) {
        std::string ch = event.character();
        if (!ch.empty() && ch[0] >= '1' && ch[0] <= '9') {
            int idx = ch[0] - '1';
            if (idx < static_cast<int>(state.resume_items.size())) {
                auto sid = state.resume_items[idx].id;
                auto cb = state.resume_callback;
                state.resume_picker_active = false;
                state.resume_items.clear();
                state.resume_view_offset = 0;
                state.resume_callback = nullptr;
                state.input_text.clear(); state.pasted_texts.clear();
                state.input_cursor = 0;
                state.clear_input_selection();
                if (cb) cb(sid);
                viewport.clamp_focus(state);
                screen.post_event(Event::Custom);
            }
        }
        return InputDisposition::Consumed;
    }
    // /model picker:数字键 1-9 直接跳到对应行(选中,不立即提交 ——
    // 和 /resume 不同,模型切换是有副作用的操作,留 Enter 确认)。
    // 其它字符 absorb,不写进 input_text。
    if (state.model_picker_open) {
        std::string ch = event.character();
        if (!ch.empty() && ch[0] >= '1' && ch[0] <= '9') {
            int idx = ch[0] - '1';
            if (idx < static_cast<int>(state.model_picker_options.size())) {
                state.model_picker_selected = idx;
                state.model_picker_view_offset = acecode::tui::scroll_to_keep_visible(
                    state.model_picker_selected, state.model_picker_view_offset,
                    acecode::tui::kResumePickerVisibleRows,
                    static_cast<int>(state.model_picker_options.size()));
                screen.post_event(Event::Custom);
            }
        }
        return InputDisposition::Consumed;
    }
    // /mode picker: number keys move the highlight but still require
    // Enter, matching the confirmation behavior of /model.
    if (state.mode_picker_open) {
        const std::string ch = event.character();
        if (!ch.empty() && ch[0] >= '1' && ch[0] <= '9') {
            const int index = ch[0] - '1';
            if (index < static_cast<int>(state.mode_picker_options.size())) {
                state.mode_picker_selected = index;
                screen.post_event(Event::Custom);
            }
        }
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

InputDisposition handle_list_picker_page(TuiInputContext& context, const ftxui::Event& event) {
    return list_picker_page(context.state, context.screen, event);
}

}
