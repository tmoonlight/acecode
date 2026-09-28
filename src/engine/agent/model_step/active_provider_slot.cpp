#include "agent/agent_loop.hpp"
#include "agent/guards/doom_guard.hpp"
#include "pa/pa_context_budget.hpp"
#include "permissions/interaction_mode.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "session/session_storage.hpp"
#include "session/token_tracker.hpp"
#include "utils/encoding.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode {

void AgentLoop::set_active_provider_for_retry(
    const std::shared_ptr<LlmProvider>& provider) {
    std::lock_guard<std::mutex> lock(active_provider_mu_);
    active_provider_ = provider;
}

void AgentLoop::clear_active_provider_for_retry(
    const std::shared_ptr<LlmProvider>& provider) {
    std::lock_guard<std::mutex> lock(active_provider_mu_);
    auto active = active_provider_.lock();
    if (!active || active == provider) {
        active_provider_.reset();
    }
}

void AgentLoop::wake_active_provider_retry() {
    std::shared_ptr<LlmProvider> provider;
    {
        std::lock_guard<std::mutex> lock(active_provider_mu_);
        provider = active_provider_.lock();
    }
    if (provider) provider->wake_retry_waiter();
}

} // namespace acecode
