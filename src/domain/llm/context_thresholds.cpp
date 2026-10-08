#include "context_thresholds.hpp"

#include <algorithm>
#include <limits>

namespace acecode {

int get_effective_context_window(int context_window) {
    if (context_window <= 0) return 0;
    const long long effective =
        static_cast<long long>(context_window) *
        EFFECTIVE_CONTEXT_WINDOW_PERCENT / 100;
    return effective > std::numeric_limits<int>::max()
        ? std::numeric_limits<int>::max()
        : static_cast<int>(effective);
}

int get_auto_compact_threshold(int context_window) {
    if (context_window <= 0) return 0;
    const long long automatic =
        static_cast<long long>(context_window) *
        AUTO_COMPACT_CONTEXT_WINDOW_PERCENT / 100;
    return std::max(MIN_AUTO_COMPACT_TOKENS, std::min(
        get_effective_context_window(context_window),
        automatic > std::numeric_limits<int>::max()
            ? std::numeric_limits<int>::max()
            : static_cast<int>(automatic)));
}

bool should_auto_compact(int context_window,
    int server_total_tokens,
    int current_request_estimated_tokens) {
    const int threshold = get_auto_compact_threshold(context_window);
    if (context_window <= 0) return false;
    return std::max(server_total_tokens, current_request_estimated_tokens) >=
           threshold;
}

TokenWarningState calculate_token_warning_state(int estimated_tokens,
                                                 int context_window) {
    TokenWarningState state;
    const int effective = get_effective_context_window(context_window);
    if (effective <= 0) {
        state.percent_left = 0.0;
        state.is_above_warning = true;
        state.is_above_error = true;
        state.is_above_auto_compact = true;
        return state;
    }

    const int remaining = effective - estimated_tokens;
    state.percent_left =
        static_cast<double>(remaining) / static_cast<double>(effective) * 100.0;
    state.is_above_warning = remaining < 20000;
    state.is_above_error = remaining < 5000;
    state.is_above_auto_compact =
        estimated_tokens >= get_auto_compact_threshold(context_window);
    return state;
}

} // namespace acecode
