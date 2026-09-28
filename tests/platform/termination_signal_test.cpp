#include <gtest/gtest.h>
#include "platform/termination_signal.hpp"
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
} // namespace
