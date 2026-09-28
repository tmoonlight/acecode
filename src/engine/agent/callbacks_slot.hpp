#pragma once
#include "agent/agent_callbacks.hpp"
#include <memory>
#include <utility>

namespace acecode {
// A publication never mutates callbacks currently being delivered. Callers take
// one value snapshot and invoke outside the publication boundary, so callbacks
// may publish their replacement or reenter the loop without taking a slot lock.
class CallbacksSlot {
public:
    explicit CallbacksSlot(AgentCallbacks callbacks = {})
        : current_(std::make_shared<const AgentCallbacks>(std::move(callbacks))) {}
    CallbacksSlot(const CallbacksSlot&) = delete;
    CallbacksSlot& operator=(const CallbacksSlot&) = delete;
    void publish(AgentCallbacks callbacks) {
        std::atomic_store(&current_,
            std::make_shared<const AgentCallbacks>(std::move(callbacks)));
    }
    AgentCallbacks snapshot() const { return *std::atomic_load(&current_); }
private:
    // Shared by the publisher and in-flight readers of this immutable version.
    std::shared_ptr<const AgentCallbacks> current_;
};
} // namespace acecode
