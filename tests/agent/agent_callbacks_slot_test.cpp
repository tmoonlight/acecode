#include <gtest/gtest.h>
#include "agent/callbacks_slot.hpp"
#include "utils/joining_thread.hpp"
#include <atomic>
#include <memory>

TEST(CallbacksSlot, ReplacementDuringDeliveryKeepsAdmittedSnapshotAlive) {
    // 展示回调发布新版本时不能锁住自己;旧的整份快照仍可完成在途投递。
    auto slot = std::make_shared<acecode::CallbacksSlot>();
    auto old_calls = std::make_shared<int>(0);
    auto new_calls = std::make_shared<int>(0);
    acecode::AgentCallbacks next;
    next.on_busy_changed = [new_calls](bool) { ++*new_calls; };
    acecode::AgentCallbacks first;
    first.on_busy_changed = [weak = std::weak_ptr<acecode::CallbacksSlot>(slot), next, old_calls](bool) {
        ++*old_calls;
        if (auto owner = weak.lock()) owner->publish(next);
    };
    slot->publish(first);
    const auto admitted = slot->snapshot();
    admitted.on_busy_changed(true);
    slot->snapshot().on_busy_changed(false);
    admitted.on_busy_changed(false);
    EXPECT_EQ(*old_calls, 2);
    EXPECT_EQ(*new_calls, 1);
}
TEST(CallbacksSlot, ConcurrentPublicationNeverExposesPartiallyAssignedFunctions) {
    // 工作者读回调与 UI 三次安装回调并行时,同一快照的两个字段必须属于同一版本。
    auto slot = std::make_shared<acecode::CallbacksSlot>();
    auto stop = std::make_shared<std::atomic<bool>>(false);
    acecode::JoiningThread publisher([slot, stop] {
        for (int i = 0; i < 1000; ++i) {
            acecode::AgentCallbacks next;
            if (i % 2) {
                next.on_busy_changed = [](bool) {};
                next.on_turn_finished = [](const std::string&) {};
            }
            slot->publish(std::move(next));
        }
        stop->store(true);
    });
    do {
        const auto current = slot->snapshot();
        EXPECT_EQ(bool(current.on_busy_changed), bool(current.on_turn_finished));
    } while (!stop->load());
    publisher.join();
}
