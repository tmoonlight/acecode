#pragma once
#include "tui/tui_state.hpp"
#include "tui/chat/chat_viewport.hpp"
#include <atomic>
#include <chrono>
namespace acecode::tui {
struct AnimationTickResult { bool immediate = false; bool should_post = false; };
// Caller holds state.mu; time is sampled only after taking the original lock.
AnimationTickResult animation_tick_locked(TuiState& state, ChatViewport& viewport,
    std::chrono::steady_clock::time_point now);
void advance_animation_phase(std::atomic<int>& anim_tick,
    std::chrono::steady_clock::time_point& last_legacy_tick_at,
    std::chrono::steady_clock::time_point tick_now, std::chrono::milliseconds legacy_tick_period);
}
