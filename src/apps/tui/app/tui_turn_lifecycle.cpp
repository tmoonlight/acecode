#include "tui/app/tui_turn_lifecycle.hpp"
#include "tui/tui_state.hpp"
#include "tui/chat/chat_viewport.hpp"
#include "tui/model/user_turn_state.hpp"
#include "tui/model/thinking_phrases.hpp"
#include "tui/model/turn_lifecycle_rules.hpp"
#include "platform/power_inhibitor.hpp"
#include "platform/native_ui/notifications.hpp"
#include "remote_control/remote_control_service.hpp"
#include "session/session_manager.hpp"
#include "config/config.hpp"
using ftxui::Event;
namespace acecode::tui {
TuiTurnLifecycle::TuiTurnLifecycle(TuiState& s, IScreenPort& scr, ChatViewport& v,
    ITurnSubmitter& submit, SessionManager& session, AppConfig& cfg, TurnObservation& obs,
    const std::function<void(const std::string&, std::string)>& title,
    const bool& ready, void* const& window)
    : state(s), screen(scr), viewport(v), submitter(submit), session_manager(session),
      config(cfg), observation(obs), start_tui_auto_title_attempt(title),
      tui_notifications_ready(ready), tui_notification_window(window) {}
std::function<void(bool)> TuiTurnLifecycle::busy_callback() {
    return [ref = lifetime_.ref(*this)](bool busy) {
        ref.with([&](TuiTurnLifecycle& owner) { owner.busy_changed(busy); });
    };
}
std::function<void(const std::string&)> TuiTurnLifecycle::title_finished_callback() {
    return [ref = lifetime_.ref(*this)](const std::string& status) {
        ref.with([&](TuiTurnLifecycle& owner) { owner.title_finished(status); });
    };
}
void TuiTurnLifecycle::title_finished(const std::string& status) {
    (void)agent();
    {
        std::lock_guard<std::mutex> lk(state.mu);
        observation.outcome = status;
    }
    const std::string session_id = session_manager.current_session_id();
    auto retry = session_manager.mark_auto_title_turn_finished(status);
    if (retry.has_value() && !session_id.empty()) {
        start_tui_auto_title_attempt(session_id, std::move(*retry));
    }
}
void TuiTurnLifecycle::busy_changed(bool busy) {
    (void)agent();
    acecode::note_process_session_busy(kTuiMainPowerSessionId, busy);
    std::unique_lock<std::mutex> lk(state.mu);
    if (busy && !state.is_waiting) {
        observation.assistant_text.clear();
        observation.outcome.clear();
        tui::begin_user_turn_locked(state, tui::UserTurnPhrase::Random, tui::WaitingUpdate::Preserve);
    }
    const bool was_waiting = state.is_waiting;
    state.is_waiting = busy;
    if (!busy) state.is_compacting = false;
    std::optional<acecode::desktop::NotifyPayload> completion_notification;
    // inline-thinking-heartbeat:回合正常收尾时追加显示端伪行
    // "● Done for Ns"(只进 state.conversation,不进 LLM context、不进
    // session JSONL,resume 后自然消失)。用户 Esc/Ctrl+C 中断的回合与
    // <1s 的瞬时回合不追加(前者已有中断反馈,后者纯噪音)。中断标记
    // 无论是否追加都在此消费复位。
    if (was_waiting && !busy) {
        const bool interrupted = state.turn_interrupted_by_user;
        state.turn_interrupted_by_user = false;
        if (tui::should_notify_turn_completion(
                interrupted, observation.outcome, tui_notifications_ready,
                !observation.assistant_text.empty(), config.desktop.notifications.enabled,
                config.desktop.notifications.on_completion) &&
            !(config.desktop.notifications.suppress_when_focused &&
              acecode::desktop::notification_window_is_foreground(
                  tui_notification_window))) {
            const std::string session_id =
                session_manager.current_session_id();
            if (!session_id.empty()) {
                completion_notification =
                    acecode::desktop::build_completion_notification(
                        session_id,
                        std::string{},
                        state.current_session_title,
                        observation.assistant_text);
            }
        }
        observation.assistant_text.clear();
        observation.outcome.clear();
        if (!interrupted &&
            state.thinking_start_time.time_since_epoch().count() != 0) {
            const auto done_secs = tui::turn_done_seconds(
                state.thinking_start_time, std::chrono::steady_clock::now());
            if (done_secs) {
                state.conversation.push_back({"turn_done",
                    "Done for " + std::to_string(*done_secs) + "s", false});
                if (state.drag_scrollbar_phase ==
                    TuiState::DragScrollbarPhase::Idle) {
                    state.chat_follow_tail = true;
                }
                viewport.clamp_focus(state);
            }
        }
    }
    // remote-control 出站:回合结束时把游标之后新增的 assistant 文本逐条
    // 转发给 IM 桥。挂在回合结束而不是 on_message:流式期间 assistant 气泡
    // 原地增量更新,逐 delta 转发会把半截文本刷给 IM。游标语义见 hub 注释。
    if (!busy) {
        auto& rc_hub = acecode::rc::remote_control_service().hub();
        if (rc_hub.enabled()) {
            std::size_t cursor = rc_hub.forward_cursor();
            // /clear 会缩短 conversation,游标越界时收口,避免越界读。
            if (cursor > state.conversation.size()) {
                cursor = state.conversation.size();
            }
            for (std::size_t i = cursor; i < state.conversation.size(); ++i) {
                const auto& m = state.conversation[i];
                if (m.role == "assistant" && !m.is_tool) {
                    rc_hub.notify_assistant_text(m.content);
                }
            }
            rc_hub.set_forward_cursor(state.conversation.size());
        }
    }
    if (!busy && !state.pending_queue.empty()) {
        std::string next_prompt = state.pending_queue.front();
        state.pending_queue.erase(state.pending_queue.begin());
        UserInput next_input;
        bool has_structured_input = false;
        if (!state.pending_structured_queue.empty() &&
            state.pending_structured_queue.front().display_text == next_prompt) {
            next_input = state.pending_structured_queue.front();
            state.pending_structured_queue.pop_front();
            has_structured_input = true;
        }
        state.conversation.push_back({"user", next_prompt, false});
        // draggable-thick-scrollbar: 用户主动拖滚动条时不要被 worker 线程
        // 强行拽回尾巴 —— 让用户看着自己挑的位置,直到他自己释放鼠标。
        // 拖到底的情况由 clamp_chat_focus 内部 (idx == last) 分支自然恢复。
        if (state.drag_scrollbar_phase ==
            TuiState::DragScrollbarPhase::Idle) {
            state.chat_follow_tail = true;
        }
        viewport.clamp_focus(state);
        tui::begin_user_turn_locked(state, tui::UserTurnPhrase::Random, tui::WaitingUpdate::SetTrue);
        lk.unlock();
        submitter.before_first_turn();
        lk.lock();
        if (has_structured_input) {
            submitter.submit_input(next_input);
        } else {
            submitter.submit_text(next_prompt);
        }
    }
    lk.unlock();
    if (completion_notification.has_value()) {
        screen.post_task([payload = std::move(*completion_notification)] {
            acecode::desktop::show_notification(payload);
        });
    }
    screen.post_event(Event::Custom);
}
}
