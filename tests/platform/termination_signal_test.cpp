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
