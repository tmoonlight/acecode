#include "tui/app/tui_overlay_gate.hpp"
#include "tui/tui_state.hpp"
#include "tui/confirm_question.hpp"
using ftxui::Event;
namespace acecode::tui {
std::function<PermissionResult(const std::string&, const std::string&)> TuiOverlayGate::confirm_callback() {
    return [ref = lifetime_.ref(*this)](const std::string& tool, const std::string& args) {
        auto result = PermissionResult::Deny;
        ref.with([&](TuiOverlayGate& gate) { result = gate.confirm(tool, args); });
        return result;
    };
}
PermissionResult TuiOverlayGate::confirm(const std::string& tool_name, const std::string& args) {
    (void)agent();  // No entry is legal before the app completes attachment.
    {
        // 子代理并发后 confirm/ask overlay 可能被别的请求占用(子会话的
        // AskUserQuestion 或远程权限确认)。占用前排队等 overlay 空闲,
        // 100ms 超时轮询让 agent_aborting 有机会打断。
        std::unique_lock<std::mutex> lk(state.mu);
        while (!agent_aborting.load() &&
               (state.confirm_pending || state.ask_pending)) {
            state.overlay_cv.wait_for(lk, std::chrono::milliseconds(100));
        }
        if (agent_aborting.load()) return PermissionResult::Deny;
        state.confirm_pending = true;
        state.confirm_tool_name = tool_name;
        state.confirm_tool_args = args;
        state.confirm_remote_session_id.clear();
        state.confirm_remote_request_id.clear();
        state.confirm_origin_label.clear();
        // 每次新确认都把焦点复位到 "No",避免上一次留下的焦点泄漏到下一次,
        // 同时安全的默认是 Deny —— 用户随手 Enter 不会误授权。
        state.confirm_focus = acecode::tui::confirm_default_focus(tool_name, args);
    }
    screen.post_event(Event::Custom);

    // Block the agent thread until the user responds in the TUI (or abort)
    std::unique_lock<std::mutex> lk(state.mu);
    state.confirm_cv.wait(lk, [&] {
        return !state.confirm_pending || agent_aborting.load();
    });
    if (agent_aborting.load()) return PermissionResult::Deny;
    return state.confirm_result;
}
}
