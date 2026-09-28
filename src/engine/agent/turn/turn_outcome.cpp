#include "agent/agent_loop.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode {

std::string AgentLoop::last_turn_error() const {
    std::lock_guard<std::mutex> lk(last_turn_error_mu_);
    return last_turn_error_;
}

void AgentLoop::record_turn_outcome(const std::string& turn_timing_status) {
    int outcome = kTurnOutcomeCompleted;
    if (turn_timing_status == "error") {
        outcome = kTurnOutcomeError;
    } else if (turn_timing_status == "aborted") {
        outcome = kTurnOutcomeAborted;
    }
    last_turn_outcome_.store(outcome, std::memory_order_release);
}

} // namespace acecode
