// 覆盖 session_host/agent_browser_store_lifecycle(openspec add-agent-browser-cookie-modes D7):
// daemon / TUI 在会话永久删除、分叉时把 Agent Browser cookie 存储的待办写进 state.json,
// 并在后台提醒 Desktop。用假提醒实现代替真实的 Desktop 代理,不连任何进程。

#include <gtest/gtest.h>

#include "ipc/agent_browser_store_state.hpp"
#include "session/session_fork_listeners.hpp"
#include "session/session_purge_listeners.hpp"
#include "session_host/agent_browser_store_lifecycle.hpp"
#include "test_support/utils/state_file_fixture.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

using acecode::AgentBrowserStoreLifecycle;
using acecode::desktop::read_agent_browser_store_state;
using acecode::test_support::StateFileTest;

namespace {

class AgentBrowserStoreLifecycleTest : public StateFileTest {};

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

// 可阻塞的假提醒:记录调用次数;gate 关着时卡在调用里,模拟 Desktop 处理得慢。
struct FakeDesktop {
    std::mutex mu;
    std::condition_variable cv;
    int calls = 0;
    bool gate_open = true;
    bool succeed = true;
    std::string error_text;

    AgentBrowserStoreLifecycle::NotifyDesktop notifier() {
        return [this](const std::atomic<bool>& abort, std::string& error) {
            std::unique_lock<std::mutex> lock(mu);
            ++calls;
            cv.notify_all();
            // abort 由被测对象置位,不会唤醒这里的条件变量,所以短间隔轮询。
            while (!gate_open && !abort.load()) {
                cv.wait_for(lock, std::chrono::milliseconds(10));
            }
            if (!succeed) error = error_text;
            return succeed;
        };
    }
    void set_gate(bool open) {
        {
            std::lock_guard<std::mutex> lock(mu);
            gate_open = open;
        }
        cv.notify_all();
    }
    bool wait_calls(int expected) {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, std::chrono::seconds(5), [this, expected] {
            return calls >= expected;
        });
    }
    int call_count() {
        std::lock_guard<std::mutex> lock(mu);
        return calls;
    }
};

} // namespace

// 场景:Desktop 开着,用户在 Web 上永久删除一个会话(purge_session_files 成功后发通知)。
// 期望:待删除清单里有这个会话,并且后台向 Desktop 发了一次 drain_store_tasks 提醒。
TEST_F(AgentBrowserStoreLifecycleTest, PurgeRecordsDeletionAndNotifiesDesktop) {
    FakeDesktop desktop;
    AgentBrowserStoreLifecycle lifecycle(desktop.notifier());
    acecode::notify_session_purged("project", "20261021_101010_abcd");

    EXPECT_TRUE(contains(read_agent_browser_store_state().pending_deletions,
                         "20261021_101010_abcd"));
    ASSERT_TRUE(desktop.wait_calls(1));
    EXPECT_TRUE(lifecycle.wait_until_idle_for_test(std::chrono::seconds(5)));
}

// 场景:把会话 A 分叉成 C(fork_session_to_new_id 成功后发通知)。
// 期望:记下 C ← A 的复制记录;分叉不需要提醒 Desktop(复制发生在 C 第一次打开浏览器时)。
TEST_F(AgentBrowserStoreLifecycleTest, ForkRecordsSeedWithoutNotifying) {
    FakeDesktop desktop;
    AgentBrowserStoreLifecycle lifecycle(desktop.notifier());
    acecode::notify_session_forked("project", "C", "A");

    const auto state = read_agent_browser_store_state();
    ASSERT_EQ(state.pending_seeds.size(), 1u);
    EXPECT_EQ(state.pending_seeds[0].session_id, "C");
    EXPECT_EQ(state.pending_seeds[0].from, "A");
    EXPECT_GT(state.pending_seeds[0].at_ms, 0);
    EXPECT_TRUE(lifecycle.wait_until_idle_for_test(std::chrono::milliseconds(200)));
    EXPECT_EQ(desktop.call_count(), 0);
}

// 场景:Desktop 没开,或是旧版 Desktop(代理回复 operation is invalid)。
// 期望:删除照常记录、不报错、不重试刷屏 —— 下次 Desktop 启动时从状态文件里处理。
TEST_F(AgentBrowserStoreLifecycleTest, DesktopUnavailableIsNotAnError) {
    FakeDesktop desktop;
    desktop.succeed = false;
    desktop.error_text = "Agent Browser proxy operation is invalid";
    AgentBrowserStoreLifecycle lifecycle(desktop.notifier());
    acecode::notify_session_purged("project", "S1");
    ASSERT_TRUE(desktop.wait_calls(1));
    EXPECT_TRUE(lifecycle.wait_until_idle_for_test(std::chrono::seconds(5)));
    EXPECT_EQ(desktop.call_count(), 1);
    EXPECT_TRUE(contains(read_agent_browser_store_state().pending_deletions, "S1"));
}

// 场景:永久删除一个带子代理的会话,会话树按「子在前、主在后」连续删除好几个。
// 期望:状态文件里每个都记下;提醒合并 —— Desktop 还在处理第一次提醒时到来的删除只再
// 提醒一次,不会一个删除一次网络往返。
TEST_F(AgentBrowserStoreLifecycleTest, BurstOfPurgesCoalescesNotifications) {
    FakeDesktop desktop;
    desktop.set_gate(false);
    AgentBrowserStoreLifecycle lifecycle(desktop.notifier());
    acecode::notify_session_purged("project", "child-1");
    ASSERT_TRUE(desktop.wait_calls(1));
    acecode::notify_session_purged("project", "child-2");
    acecode::notify_session_purged("project", "main");
    desktop.set_gate(true);
    EXPECT_TRUE(lifecycle.wait_until_idle_for_test(std::chrono::seconds(5)));
    EXPECT_EQ(desktop.call_count(), 2);
    const auto state = read_agent_browser_store_state();
    EXPECT_TRUE(contains(state.pending_deletions, "child-1"));
    EXPECT_TRUE(contains(state.pending_deletions, "child-2"));
    EXPECT_TRUE(contains(state.pending_deletions, "main"));
}

// 场景:服务停止(daemon / TUI 退出)之后又有会话被删除或分叉。
// 期望:监听已注销,不再写状态文件;停止时卡在提醒里的调用被中止,stop 不会挂住。
TEST_F(AgentBrowserStoreLifecycleTest, StopUnregistersListenersAndAbortsPendingNotify) {
    FakeDesktop desktop;
    desktop.set_gate(false);
    AgentBrowserStoreLifecycle lifecycle(desktop.notifier());
    acecode::notify_session_purged("project", "before-stop");
    ASSERT_TRUE(desktop.wait_calls(1));
    const auto started = std::chrono::steady_clock::now();
    lifecycle.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));

    acecode::notify_session_purged("project", "after-stop");
    acecode::notify_session_forked("project", "C2", "A2");
    const auto state = read_agent_browser_store_state();
    EXPECT_TRUE(contains(state.pending_deletions, "before-stop"));
    EXPECT_FALSE(contains(state.pending_deletions, "after-stop"));
    EXPECT_TRUE(state.pending_seeds.empty());
}
