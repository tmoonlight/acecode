#pragma once

#include "llm/llm_provider.hpp"

#include <memory>
#include <mutex>
#include <utility>

namespace acecode::agent {

// Leaf lock: take a lease under mu_, invoke provider code only after unlocking.
class ActiveProviderSlot {
public:
    void publish(const std::shared_ptr<LlmProvider>& provider);
    void clear(const std::shared_ptr<LlmProvider>& provider);
    void wake();

private:
    std::mutex mu_;
    std::weak_ptr<LlmProvider> provider_;
};

class ActiveProviderScope {
public:
    ActiveProviderScope(ActiveProviderSlot& slot, std::shared_ptr<LlmProvider> provider)
        : slot_(slot), provider_(std::move(provider)) { slot_.publish(provider_); }
    ~ActiveProviderScope() { reset(); }
    ActiveProviderScope(const ActiveProviderScope&) = delete;
    ActiveProviderScope& operator=(const ActiveProviderScope&) = delete;
    void reset() {
        if (!active_) return;
        active_ = false;
        slot_.clear(provider_);
    }

private:
    ActiveProviderSlot& slot_; // Borrowed within the owning worker call.
    // The scope shares the active provider with the accessor during this call.
    std::shared_ptr<LlmProvider> provider_;
    bool active_ = true;
};

} // namespace acecode::agent
