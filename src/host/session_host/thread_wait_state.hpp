#pragma once
#include "thread_service.hpp"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace acecode { struct SessionEntry; }
namespace acecode::thread_detail {
// The waiting call owns target sessions; event listeners retain this state weakly.
struct WaitTargetState {
    ThreadWaitTarget target;
    std::shared_ptr<SessionEntry> active;
    std::uint64_t cursor = 0;
    bool terminal = false;
    nlohmann::json event;
};
struct WaitState {
    std::vector<WaitTargetState> states;
    std::mutex mu;
    std::condition_variable cv;
    bool ready = false;
};
} // namespace acecode::thread_detail
