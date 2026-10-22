#include "agent_browser_store_lifecycle.hpp"

#include "ipc/agent_browser_store_state.hpp"
#include "session/session_fork_listeners.hpp"
#include "session/session_purge_listeners.hpp"
#include "tool/agent_browser/cdp_client.hpp"
#include "utils/logger.hpp"

#include <chrono>
#include <utility>

namespace acecode {
namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Desktop 没开时 runtime manifest 不存在,connect 立即失败;开着时一次本机管道往返。
bool notify_desktop_over_proxy(const std::atomic<bool>& abort, std::string& error) {
    agent_browser::AgentBrowserCdpClient client;
    if (!client.connect(std::chrono::seconds(2), &abort, error)) return false;
    return client.drain_store_tasks(std::chrono::seconds(5), &abort, error);
}

} // namespace

AgentBrowserStoreLifecycle::AgentBrowserStoreLifecycle(NotifyDesktop notify)
    : notify_(notify ? std::move(notify) : NotifyDesktop(notify_desktop_over_proxy)),
      notifier_(&AgentBrowserStoreLifecycle::notify_loop, this) {
    const auto owner = lifetime_.ref(*this);
    purge_listener_ = add_session_purge_listener(
        [owner](const std::string& /*project_dir*/, const std::string& session_id) {
            owner.with([&session_id](AgentBrowserStoreLifecycle& self) {
                self.on_session_purged(session_id);
            });
        });
    fork_listener_ = add_session_fork_listener(
        [owner](const std::string& /*project_dir*/,
                const std::string& new_session_id,
                const std::string& forked_from_id) {
            owner.with([&new_session_id, &forked_from_id](AgentBrowserStoreLifecycle& self) {
                self.on_session_forked(new_session_id, forked_from_id);
            });
        });
}

AgentBrowserStoreLifecycle::~AgentBrowserStoreLifecycle() {
    stop();
}

void AgentBrowserStoreLifecycle::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopped_) return;
        stopped_ = true;
    }
    remove_session_purge_listener(purge_listener_);
    remove_session_fork_listener(fork_listener_);
    lifetime_.revoke();
    abort_.store(true);
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    notifier_.join();
}

bool AgentBrowserStoreLifecycle::wait_until_idle_for_test(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout,
                        [&pending = notify_pending_, &notifying = notifying_] {
                            return !pending && !notifying;
                        });
}

void AgentBrowserStoreLifecycle::on_session_purged(const std::string& session_id) {
    if (!desktop::record_agent_browser_store_deletion(session_id)) {
        LOG_WARN("[agent-browser] failed to record cookie store deletion for " + session_id);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        notify_pending_ = true;
    }
    cv_.notify_all();
}

void AgentBrowserStoreLifecycle::on_session_forked(const std::string& new_session_id,
                                                   const std::string& forked_from_id) {
    if (!desktop::record_agent_browser_store_seed(new_session_id, forked_from_id, now_ms())) {
        LOG_WARN("[agent-browser] failed to record cookie store fork " + forked_from_id +
                 " -> " + new_session_id);
    }
}

void AgentBrowserStoreLifecycle::notify_loop() {
    std::unique_lock<std::mutex> lock(mu_);
    while (true) {
        cv_.wait(lock, [&pending = notify_pending_, &stopping = stopping_] {
            return pending || stopping;
        });
        if (stopping_) return;
        // 一次提醒处理状态文件里全部待删除项,期间到来的删除合并到下一次。
        notify_pending_ = false;
        notifying_ = true;
        lock.unlock();
        std::string error;
        const bool ok = notify_(abort_, error);
        if (ok) {
            LOG_INFO("[agent-browser] asked Desktop to delete removed session cookie stores");
        } else {
            LOG_DEBUG("[agent-browser] Desktop not notified about removed cookie stores "
                      "(deleted at its next start): " + error);
        }
        lock.lock();
        notifying_ = false;
        cv_.notify_all();
    }
}

} // namespace acecode
