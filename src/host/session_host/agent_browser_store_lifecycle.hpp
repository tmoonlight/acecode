#pragma once

#include "utils/joining_thread.hpp"
#include "utils/lifetime_token.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace acecode {

// Agent Browser 按会话 cookie 存储的生命周期(openspec add-agent-browser-cookie-modes D7)。
// 会话的永久删除与分叉发生在 daemon / TUI,cookie 存储却在 Desktop 进程里,所以这里
// 只把待办写进共享的 state.json(agent_browser 段),由 Desktop 启动时或收到提醒时处理:
//   - 分叉:记一条 pending_seeds(子会话第一次打开浏览器时从父会话复制 cookie);
//   - 永久删除:记一条 pending_deletions,再在后台线程经 Agent Browser 代理发一次
//     drain_store_tasks 提醒(Desktop 没开 / 旧版 Desktop 不认识都只记 debug 日志,
//     下次 Desktop 启动时照样会删)。
// 监听回调只写状态文件(很快,不碰网络),提醒在自有线程里做,连续删除合并成一次提醒。
class AgentBrowserStoreLifecycle {
public:
    // 发提醒的实现,测试可注入假实现;abort 在 stop() 时置位,实现应尽快返回。
    // 返回 false 时 error 说明原因。
    using NotifyDesktop =
        std::function<bool(const std::atomic<bool>& abort, std::string& error)>;

    explicit AgentBrowserStoreLifecycle(NotifyDesktop notify = {});
    ~AgentBrowserStoreLifecycle();

    AgentBrowserStoreLifecycle(const AgentBrowserStoreLifecycle&) = delete;
    AgentBrowserStoreLifecycle& operator=(const AgentBrowserStoreLifecycle&) = delete;

    // 注销监听、等在途回调结束、停掉提醒线程。析构时自动调用,可重复调用。
    void stop();

    // 测试用:等到当前没有待发的提醒(或超时)。
    bool wait_until_idle_for_test(std::chrono::milliseconds timeout);

private:
    void on_session_purged(const std::string& session_id);
    void on_session_forked(const std::string& new_session_id,
                           const std::string& forked_from_id);
    void notify_loop();

    NotifyDesktop notify_;
    std::uint64_t purge_listener_ = 0;
    std::uint64_t fork_listener_ = 0;
    std::mutex mu_;
    std::condition_variable cv_;
    bool notify_pending_ = false;
    bool notifying_ = false;
    bool stopping_ = false;
    bool stopped_ = false;
    std::atomic<bool> abort_{false};
    JoiningThread notifier_;
    LifetimeToken lifetime_;
};

} // namespace acecode
