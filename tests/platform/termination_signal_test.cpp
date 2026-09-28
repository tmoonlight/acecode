#include <gtest/gtest.h>
#include "platform/termination_signal.hpp"
#include "platform/process/graceful_stop.hpp"
#include "utils/joining_thread.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <memory>

namespace {
using acecode::platform::TerminationSignal;
using namespace std::chrono_literals;

// 场景与期望：终止请求应唤醒等待者并保持终止状态，避免一次唤醒被后续等待遗漏。
TEST(TerminationSignal, RequestWakesWaiterAndRemainsRequested) {
    auto signal = std::make_shared<TerminationSignal>();
    auto result = std::make_shared<std::promise<bool>>();
    auto finished = result->get_future();
    EXPECT_FALSE(signal->wait_for(0ms));
    acecode::JoiningThread waiter([signal, result] { result->set_value(signal->wait_for(2s)); });
    signal->request();
    ASSERT_EQ(finished.wait_for(3s), std::future_status::ready);
    EXPECT_TRUE(finished.get());
    EXPECT_TRUE(signal->requested());
    EXPECT_TRUE(signal->wait_for(0ms));
}

// 场景与期望：进程信号在桥接安装前后到达都应保留，避免启动窗口丢失退出请求。
TEST(TerminationSignal, ProcessBridgeDeliversRequestsBeforeAndAfterInstallation) {
    TerminationSignal::request_process_termination();
    {
        TerminationSignal signal;
        signal.install_process_handlers();
        EXPECT_TRUE(signal.wait_for(0ms));
        signal.install_process_handlers();
        TerminationSignal competing;
        EXPECT_THROW(competing.install_process_handlers(), std::logic_error);
    }
    {
        TerminationSignal replacement;
        replacement.install_process_handlers();
        EXPECT_FALSE(replacement.requested());
        TerminationSignal::request_process_termination();
        EXPECT_TRUE(replacement.wait_for(100ms));
    }
}

// 场景与期望：并发投递信号时注销桥接必须等待在途调用，避免句柄释放后仍被访问。
TEST(TerminationSignal, UnregistrationWaitsForConcurrentBridgeCalls) {
    auto signal = std::make_unique<TerminationSignal>();
    signal->install_process_handlers();
    auto entered = std::make_shared<std::promise<void>>();
    auto started = entered->get_future();
    acecode::JoiningThread requester([entered](acecode::StopToken stop) {
        entered->set_value();
        while (!stop.stop_requested()) TerminationSignal::request_process_termination();
    });
    ASSERT_EQ(started.wait_for(2s), std::future_status::ready);
    signal.reset();
    requester.request_stop();
    requester.join();
    // Consume requests sent after unregistration without leaving a process
    // bridge owner or pending notification for another test.
    TerminationSignal drain;
    drain.install_process_handlers();
}
#ifdef _WIN32
// 场景与期望：没有控制台的 daemon 也能收到停止请求;端点只存在于宿主寿命内,
// 请求后保持终止态,替换宿主不会继承已经关闭的旧事件。
TEST(TerminationSignal, ProcessStopEndpointWakesOwnerAndIsReleasedOnDestruction) {
    using acecode::platform::request_process_stop;
    EXPECT_FALSE(request_process_stop(nullptr));
    EXPECT_FALSE(request_process_stop(::GetCurrentProcess()));
    {
        TerminationSignal signal;
        signal.install_process_handlers();
        EXPECT_FALSE(signal.wait_for(0ms));
        ASSERT_TRUE(request_process_stop(::GetCurrentProcess()));
        EXPECT_TRUE(signal.wait_for(100ms));
        EXPECT_TRUE(signal.requested());
        EXPECT_TRUE(signal.wait_for(0ms));
    }
    EXPECT_FALSE(request_process_stop(::GetCurrentProcess()));
    {
        TerminationSignal replacement;
        replacement.install_process_handlers();
        EXPECT_FALSE(replacement.wait_for(0ms));
    }
}

// 场景与期望：在途发送只持有内核事件租约;宿主析构后请求不能访问已释放的
// C++ 对象,也不能意外重建不存在的端点。
TEST(TerminationSignal, ExternalRequestsCanRaceWithOwnerDestruction) {
    auto owner = std::make_unique<TerminationSignal>();
    owner->install_process_handlers();
    auto entered = std::make_shared<std::promise<void>>();
    auto ready = entered->get_future();
    acecode::JoiningThread requester([entered](acecode::StopToken stop) {
        entered->set_value();
        while (!stop.stop_requested())
            acecode::platform::request_process_stop(::GetCurrentProcess());
    });
    ASSERT_EQ(ready.wait_for(2s), std::future_status::ready);
    owner.reset();
    requester.request_stop();
    requester.join();
    EXPECT_FALSE(acecode::platform::request_process_stop(::GetCurrentProcess()));
}
#endif
} // namespace
