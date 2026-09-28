#include <gtest/gtest.h>
#include "session/event_dispatcher.hpp"
#include "session/scoped_subscription.hpp"
#include "utils/joining_thread.hpp"
#include "utils/scope_exit.hpp"
#include <chrono>
#include <future>
#include <memory>
#include <type_traits>

namespace {
using namespace std::chrono_literals;
struct DeliveryGate {
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
};
TEST(EventDispatcherWait, WaitsForAdmittedListenerAndSuppressesLaterEvents) {
    // 修复前 unsubscribe 只从表中删除,已经复制的 listener 仍可访问已析构的宿主。
    auto dispatcher = std::make_shared<acecode::EventDispatcher>();
    auto gate = std::make_shared<DeliveryGate>();
    auto entered = gate->entered.get_future();
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto id = dispatcher->subscribe([gate, calls](const auto&) {
        ++*calls;
        gate->entered.set_value();
        gate->released.wait_for(2s);
    });
    acecode::ScopeExit release([gate] {
        try { gate->release.set_value(); } catch (const std::future_error&) {}
    });
    acecode::JoiningThread emit([dispatcher] { dispatcher->emit(acecode::SessionEventKind::Done, {}); });
    ASSERT_EQ(entered.wait_for(2s), std::future_status::ready);
    auto finished = std::make_shared<std::promise<void>>();
    auto done = finished->get_future();
    acecode::JoiningThread remove([dispatcher, id, finished] {
        dispatcher->unsubscribe_and_wait(id);
        finished->set_value();
    });
    EXPECT_EQ(done.wait_for(20ms), std::future_status::timeout);
    gate->release.set_value();
    EXPECT_EQ(done.wait_for(2s), std::future_status::ready);
    emit.join();
    remove.join();
    dispatcher->emit(acecode::SessionEventKind::Done, {});
    EXPECT_EQ(calls->load(), 1);
}
TEST(EventDispatcherWait, SelfUnsubscribeDoesNotWaitForItsOwnStack) {
    // listener 在自身投递线程退订必须立即返回,下一次 emit 不得再次进入。
    auto dispatcher = std::make_shared<acecode::EventDispatcher>();
    auto id = std::make_shared<acecode::SessionClient::SubscriptionId>(0);
    auto calls = std::make_shared<int>(0);
    *id = dispatcher->subscribe([weak = std::weak_ptr<acecode::EventDispatcher>(dispatcher), id, calls](const auto&) {
        ++*calls;
        if (auto owner = weak.lock()) owner->unsubscribe_and_wait(*id);
    });
    dispatcher->emit(acecode::SessionEventKind::Done, {});
    dispatcher->emit(acecode::SessionEventKind::Done, {});
    EXPECT_EQ(*calls, 1);
}
TEST(EventDispatcherWait, NestedDeliveryCanUnsubscribeSuspendedOuterListener) {
    // 两个 dispatcher 嵌套投递时,只检查最近一层会导致等待当前线程自身。
    auto outer = std::make_shared<acecode::EventDispatcher>();
    auto inner = std::make_shared<acecode::EventDispatcher>();
    auto outer_id = std::make_shared<acecode::SessionClient::SubscriptionId>(0);
    inner->subscribe([weak = std::weak_ptr<acecode::EventDispatcher>(outer), outer_id](const auto&) {
        if (auto owner = weak.lock()) owner->unsubscribe_and_wait(*outer_id);
    });
    *outer_id = outer->subscribe([inner](const auto&) { inner->emit(acecode::SessionEventKind::Done, {}); });
    outer->emit(acecode::SessionEventKind::Done, {});
    EXPECT_EQ(outer->listener_count(), 0u);
}
TEST(ScopedSubscription, MoveTransfersOneCleanupAndResetIsIdempotent) {
    // 资源转移后旧对象不得退订,新对象析构/重置只负责自己的一次订阅。
    static_assert(!std::is_copy_constructible_v<acecode::ScopedSubscription>);
    acecode::EventDispatcher dispatcher;
    {
        acecode::ScopedSubscription first(dispatcher, dispatcher.subscribe([](const auto&) {}));
        acecode::ScopedSubscription second(std::move(first));
        first.reset();
        EXPECT_EQ(dispatcher.listener_count(), 1u);
        second.reset();
        second.reset();
        EXPECT_EQ(dispatcher.listener_count(), 0u);
    }
}
}
