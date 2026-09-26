#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace acecode::test {

// 只用于新并发原语测试:显式控制执行顺序,即使实现出错也有超时退出。
class ConcurrencyGate {
public:
    void open() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            open_ = true;
        }
        cv_.notify_all();
    }

    bool wait(std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, timeout, [this] { return open_; });
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool open_ = false;
};

}  // namespace acecode::test
