#pragma once

#include "activity_narrator.hpp"
#include "session/event_dispatcher.hpp"
#include <chrono>
#include <functional>
#include <mutex>
#include <string>

namespace acecode::agent {

class AgentProgressEmitter {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    AgentProgressEmitter(ActivityNarrator& activity, EventDispatcher& events,
                         Clock clock = {})
        : activity_(activity), events_(events), clock_(std::move(clock)) {}
    void emit(const std::string& phase, const std::string& label,
              const std::string& detail = {}, const std::string& tool = {},
              const std::string& tool_call_id = {}, int tool_index = -1,
              bool force = false);
private:
    ActivityNarrator& activity_;
    EventDispatcher& events_;
    Clock clock_;
    // Leaf mutex: no event dispatch or narrator callback inside.
    std::mutex mutex_;
    std::string active_key_;
    std::int64_t started_at_ms_ = 0;
    std::chrono::steady_clock::time_point last_emit_at_{};
};

} // namespace acecode::agent
