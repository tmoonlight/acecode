#include "turn_lifecycle_rules.hpp"

namespace acecode::tui {

bool should_notify_turn_completion(bool interrupted, const std::string& outcome,
    bool ready, bool has_text, bool enabled, bool on_completion) {
    return !interrupted && outcome == "completed" && ready && has_text && enabled && on_completion;
}
std::optional<long> turn_done_seconds(std::chrono::steady_clock::time_point started,
    std::chrono::steady_clock::time_point now) {
    if (started.time_since_epoch().count() == 0) return std::nullopt;
    const long seconds = static_cast<long>(
        std::chrono::duration_cast<std::chrono::seconds>(now - started).count());
    return seconds >= 1 ? std::optional<long>(seconds) : std::nullopt;
}

} // namespace acecode::tui
