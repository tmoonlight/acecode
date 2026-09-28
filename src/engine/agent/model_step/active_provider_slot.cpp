#include "active_provider_slot.hpp"

namespace acecode::agent {

void ActiveProviderSlot::publish(const std::shared_ptr<LlmProvider>& provider) {
    std::lock_guard<std::mutex> lock(mu_);
    provider_ = provider;
}

void ActiveProviderSlot::clear(const std::shared_ptr<LlmProvider>& provider) {
    std::lock_guard<std::mutex> lock(mu_);
    auto active = provider_.lock();
    if (!active || active == provider) provider_.reset();
}

void ActiveProviderSlot::wake() {
    std::shared_ptr<LlmProvider> provider;
    {
        std::lock_guard<std::mutex> lock(mu_);
        provider = provider_.lock();
    }
    if (provider) provider->wake_retry_waiter();
}

} // namespace acecode::agent
