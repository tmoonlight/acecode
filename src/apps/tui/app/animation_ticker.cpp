#include "tui/app/animation_ticker.hpp"
#include "tui/app/animation_tick.hpp"
#include "tui/app/tui_screen_host.hpp"
namespace acecode::tui {
AnimationTicker::AnimationTicker(TuiState& state, TuiScreenHost& screen, ChatViewport& viewport,
    std::atomic<int>& anim_tick, bool conhost_compat_layout)
    : state_(state), screen_(screen), viewport_(viewport), anim_tick_(anim_tick),
      conhost_compat_layout_(conhost_compat_layout), worker_([this] { run(); }) {}
AnimationTicker::~AnimationTicker() { request_stop(); join(); }
ThinkingAnimationPacingContext AnimationTicker::pacing_context() {
    ThinkingAnimationPacingContext context;
    context.conhost_compat_layout = conhost_compat_layout_;
    context.keyboard_input_recent = is_keyboard_input_recent(monotonic_milliseconds(),
        screen_.last_keyboard_input_at_ms().load(std::memory_order_acquire));
    context.last_frame_latency_ms = screen_.redraw_pacer()->last_frame_latency_ms();
    std::lock_guard<std::mutex> lock(state_.mu);
    context.drag_autoscroll_active = state_.drag_phase == drag_scroll::Phase::ScrollingUp
        || state_.drag_phase == drag_scroll::Phase::ScrollingDown;
    context.thinking_visible = state_.is_waiting && !state_.tool_running;
    return context;
}
void AnimationTicker::run() {
    const auto period = std::chrono::milliseconds(
        conhost_compat_layout_ ? kConhostAnimationFrameMs : kDefaultAnimationFrameMs);
    auto previous = std::chrono::steady_clock::now();
    while (running_) {
        const int interval = select_animation_frame_interval_ms(pacing_context());
        std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        // Keep the original sleep/stop boundary: an admitted final tick completes.
        advance_animation_phase(anim_tick_, previous, std::chrono::steady_clock::now(), period);
        AnimationTickResult result;
        {
            std::lock_guard<std::mutex> lock(state_.mu);
            result = animation_tick_locked(state_, viewport_, std::chrono::steady_clock::now());
        }
        if (result.immediate) screen_.post_event(ftxui::Event::Custom);
        else if (result.should_post)
            screen_.request_scheduled_redraw(select_animation_frame_interval_ms(pacing_context()));
    }
}
}
