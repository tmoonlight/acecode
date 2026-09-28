#include "tui/input/tui_input_context.hpp"
#include "tui/overlays/confirm_overlay_input.hpp"
#include "tui/terminal_key_event.hpp"
#include "tui/picker_scroll.hpp"
#include <algorithm>
using ftxui::Event;
using ftxui::Mouse;
using ftxui::Box;
#include "tui/confirm_question.hpp"

namespace acecode::tui {
static bool handle_confirm_overlay_event(
    TuiState& state,
    IScreenPort& screen,
    const Event& event,
    const std::function<void(const std::string& session_id,
                             const std::string& request_id,
                             PermissionResult r)>& respond_remote = nullptr) {
    std::unique_lock<std::mutex> lk(state.mu);
    if (!state.confirm_pending) {
        return false;
    }

    auto submit = [&](PermissionResult r) {
        state.input_text.clear();
        state.pasted_texts.clear();
        state.input_cursor = 0;
        state.clear_input_selection();
        state.confirm_pending = false;
        if (!state.confirm_remote_session_id.empty()) {
            const std::string sid = state.confirm_remote_session_id;
            const std::string rid = state.confirm_remote_request_id;
            state.confirm_remote_session_id.clear();
            state.confirm_remote_request_id.clear();
            state.confirm_origin_label.clear();
            if (respond_remote) respond_remote(sid, rid, r);
        } else {
            state.confirm_result = r;
            state.confirm_cv.notify_one();
        }
        // overlay 释放:唤醒排队占用者(主会话确认 / 子会话 ask 工具)。
        state.overlay_cv.notify_all();
        screen.post_event(Event::Custom);
    };

    // 选项随 payload 变化(bash 的「只放行该目录」/「记住前缀」按需出现),
    // 数字快捷键 = 选项序号,No 永远最后。
    const auto options = acecode::tui::build_confirm_options(
        state.confirm_tool_name, state.confirm_tool_args);
    const int count = std::max(1, static_cast<int>(options.size()));
    auto has_result = [&](PermissionResult r) {
        for (const auto& option : options) if (option.result == r) return true;
        return false;
    };
    if (tui::matches_terminal_key(event, acecode::tui::TerminalKey::Escape)) {
        submit(PermissionResult::Deny);
        return true;
    }
    if (event == Event::ArrowUp) {
        state.confirm_focus = (state.confirm_focus + count - 1) % count;
        screen.post_event(Event::Custom);
        return true;
    }
    if (event == Event::ArrowDown) {
        state.confirm_focus = (state.confirm_focus + 1) % count;
        screen.post_event(Event::Custom);
        return true;
    }
    if (tui::matches_terminal_key(
            event, acecode::tui::TerminalKey::Tab, tui::kTerminalShift)) {
        if (has_result(PermissionResult::AlwaysAllow)) submit(PermissionResult::AlwaysAllow);
        return true;
    }
    if (event == Event::Return) {
        const int f = std::clamp(state.confirm_focus, 0, count - 1);
        submit(options.empty() ? PermissionResult::Deny : options[static_cast<std::size_t>(f)].result);
        return true;
    }
    if (event.is_character()) {
        const std::string& c = event.character();
        if (c.size() == 1 && c[0] >= '1' && c[0] <= '9') {
            const std::size_t index = static_cast<std::size_t>(c[0] - '1');
            if (index < options.size()) submit(options[index].result);
            return true;
        }
        if (c == "y" || c == "Y") {
            submit(PermissionResult::Allow);
            return true;
        }
        if (c == "n" || c == "N") {
            submit(PermissionResult::Deny);
            return true;
        }
        if (c == "a" || c == "A") {
            if (has_result(PermissionResult::AlwaysAllow)) submit(PermissionResult::AlwaysAllow);
            return true;
        }
        return true;
    }
    return false;
}

InputDisposition handle_confirm_overlay_input(TuiState& state, IScreenPort& screen,
    const ftxui::Event& event, const PermissionResponder& respond_remote) {
    return input_handled(handle_confirm_overlay_event(state, screen, event, respond_remote));
}

InputDisposition handle_confirm_overlay_input(TuiInputContext& context, const ftxui::Event& event) {
    return handle_confirm_overlay_input(context.state, context.screen, event, context.respond_remote);
}

InputDisposition pump_remote_confirm(TuiInputContext& context, const Event& event) {
    auto& state = context.state;
    // 远程确认泵:confirm/ask overlay 空闲时弹出子会话的权限请求占用
    // confirm overlay(带来源标注)。入队方 PostEvent(Custom) 保证本泵
    // 在请求到达后至少跑一次;overlay 释放时的 PostEvent 驱动下一条。
    {
        std::lock_guard<std::mutex> lk(state.mu);
        if (!state.confirm_pending && !state.ask_pending &&
            !state.remote_confirm_queue.empty()) {
            auto req = std::move(state.remote_confirm_queue.front());
            state.remote_confirm_queue.pop_front();
            state.confirm_pending = true;
            state.confirm_tool_name = req.tool;
            state.confirm_tool_args = req.args_preview;
            state.confirm_remote_session_id = req.session_id;
            state.confirm_remote_request_id = req.request_id;
            state.confirm_origin_label = req.origin_label;
            state.confirm_focus = acecode::tui::confirm_default_focus(req.tool, req.args_preview);
        }
    }


    return InputDisposition::Continue;
}

}
