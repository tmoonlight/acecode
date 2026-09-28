#pragma once
#include "agent/callbacks_slot.hpp"


#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace acecode::agent::detail {

std::string human_bytes(std::size_t bytes);

std::string format_bytes_detail(std::size_t bytes);

} // namespace acecode::agent::detail

namespace acecode {
struct AgentCallbacks;
struct ProviderErrorInfo;
class EventDispatcher;
namespace agent {
struct RetryProgressText {
    std::string phase;
    std::string label;
    std::string detail;
};

class RetryProgressReporter {
public:
    RetryProgressReporter(CallbacksSlot& callbacks, EventDispatcher& events)
        : callbacks_(callbacks), events_(events) {}
    void standard(const ProviderErrorInfo& info, bool waiting, bool compaction);
    void emit(const ProviderErrorInfo& info, bool waiting, RetryProgressText text);
private:
    CallbacksSlot& callbacks_;
    EventDispatcher& events_;
};
} // namespace agent
} // namespace acecode
