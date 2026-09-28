#pragma once

// 上下文窗口阈值与告警状态(P2-02 自 commands/compact.hpp 拆出):有效窗口 95%、自动压缩 90%,
// 纯算术,供 agent_loop 与压缩流程共用。

namespace acecode {

constexpr int EFFECTIVE_CONTEXT_WINDOW_PERCENT = 95;
constexpr int AUTO_COMPACT_CONTEXT_WINDOW_PERCENT = 90;

struct TokenWarningState {
    double percent_left = 100.0;
    bool is_above_warning = false;
    bool is_above_error = false;
    bool is_above_auto_compact = false;
};

int get_effective_context_window(int context_window);
int get_auto_compact_threshold(int context_window);

bool should_auto_compact(int context_window,
                         int server_total_tokens,
                         int current_request_estimated_tokens);

TokenWarningState calculate_token_warning_state(int estimated_tokens,
                                                 int context_window);

} // namespace acecode
