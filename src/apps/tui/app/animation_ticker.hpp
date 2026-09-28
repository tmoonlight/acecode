#pragma once
#include "tui/thinking_animation.hpp"
#include "utils/joining_thread.hpp"
namespace acecode { struct TuiState; }
namespace acecode::tui {
class TuiScreenHost; class ChatViewport;
class AnimationTicker {
public:
    AnimationTicker(TuiState& state, TuiScreenHost& screen, ChatViewport& viewport,
        std::atomic<int>& anim_tick, bool conhost_compat_layout);
    ~AnimationTicker();
    void request_stop() { running_.store(false); }
    void join() { worker_.join(); }
private:
    ThinkingAnimationPacingContext pacing_context();
    void run();
    TuiState& state_;
    TuiScreenHost& screen_;
    ChatViewport& viewport_;
    std::atomic<int>& anim_tick_;
    const bool conhost_compat_layout_;
    std::atomic<bool> running_{true};
    JoiningThread worker_;  // Joins before dependencies; owns the this capture.
};
}
