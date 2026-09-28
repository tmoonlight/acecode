#pragma once

// 重试等待器:provider 重试期间在条件变量上睡眠而不是轮询,AgentLoop 的 abort() / shutdown()
// 用 wake() 立即唤醒。它是 LlmProvider 的成员类型,所以随 P2-02 从 provider/retry_policy 下沉到
// domain 层的 llm 模块(retry_policy 的退避计算与 HTTP 判定仍留在 provider)。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace acecode {

// A retry wait that sleeps on a condition variable rather than polling. wake()
// is used by AgentLoop::abort()/shutdown() and can also trigger an immediate
// retry when no abort has been requested.
class ProviderRetryWaiter {
public:
    bool wait_for(std::chrono::milliseconds delay,
                  const std::atomic<bool>* abort_flag);
    void wake();
    // Notify request-owned abort flags without shortening another request's
    // retry delay on a shared provider.
    void notify_cancelled_request();

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::uint64_t wake_generation_ = 0;
};

} // namespace acecode
