#pragma once
#include <chrono>
#include <optional>
#include <string>
namespace acecode::tui {
bool should_notify_turn_completion(bool interrupted, const std::string& outcome,
    bool ready, bool has_text, bool enabled, bool on_completion);
std::optional<long> turn_done_seconds(std::chrono::steady_clock::time_point started,
    std::chrono::steady_clock::time_point now);
}
