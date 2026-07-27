#include "tui/redraw_pacer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace acecode { namespace tui {

// 场景:高频 thinking tick / delta 在一个已接受帧完成前持续到达。
// 期望:无论顺序调用多少次,都只有首个请求取得 generation。
TEST(TuiRedrawPacerTest, CoalescesRepeatedRequestsUntilFrameCompletes) {
    TuiRedrawPacer pacer;

    EXPECT_TRUE(pacer.try_request_scheduled_redraw(1000, 50));
    for (int i = 0; i < 10000; ++i) {
        EXPECT_FALSE(pacer.try_request_scheduled_redraw(1001 + i, 0));
    }
    EXPECT_TRUE(pacer.scheduled_redraw_pending());
    EXPECT_EQ(pacer.requested_generation(), 1u);
    EXPECT_EQ(pacer.completed_generation(), 0u);

    const auto ticket = pacer.begin_frame(1100);
    EXPECT_EQ(ticket.scheduled_generation, 1u);
    pacer.complete_frame(ticket, 1125);

    EXPECT_FALSE(pacer.scheduled_redraw_pending());
    EXPECT_EQ(pacer.completed_generation(), 1u);
    EXPECT_EQ(pacer.last_frame_latency_ms(), 25);
    EXPECT_EQ(pacer.last_frame_completed_at_ms(), 1125);
}

// 场景:不同线程同时提交大量可合并刷新。
// 期望:原子 generation 门只接受一次,不存在检查后写入竞争。
TEST(TuiRedrawPacerTest, ConcurrentRequestStormAcceptsOneGeneration) {
    TuiRedrawPacer pacer;
    std::atomic<int> accepted{0};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&pacer, &accepted, worker] {
            for (int i = 0; i < 1000; ++i) {
                if (pacer.try_request_scheduled_redraw(
                        1000 + worker * 1000 + i, 0)) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(accepted.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(pacer.requested_generation(), 1u);
    EXPECT_EQ(pacer.completed_generation(), 0u);
}

// 场景:一个直接输入帧已取到旧 generation,随后后台刷新才被接受。
// 期望:旧帧完成不能清掉较新的请求;新帧拿到该 generation 后才能完成它。
TEST(TuiRedrawPacerTest, OlderFrameCannotCompleteNewerRequest) {
    TuiRedrawPacer pacer;
    const auto older_frame = pacer.begin_frame(100);
    ASSERT_EQ(older_frame.scheduled_generation, 0u);

    ASSERT_TRUE(pacer.try_request_scheduled_redraw(101, 0));
    pacer.complete_frame(older_frame, 110);

    EXPECT_TRUE(pacer.scheduled_redraw_pending());
    EXPECT_EQ(pacer.requested_generation(), 1u);
    EXPECT_EQ(pacer.completed_generation(), 0u);

    const auto requested_frame = pacer.begin_frame(111);
    ASSERT_EQ(requested_frame.scheduled_generation, 1u);
    pacer.complete_frame(requested_frame, 130);

    EXPECT_FALSE(pacer.scheduled_redraw_pending());
    EXPECT_EQ(pacer.completed_generation(), 1u);
}

// 场景:上帧已经完成,但最小后台刷新间隔尚未走完。
// 期望:间隔内拒绝,恰到边界接受;直接事件无需经过此 API。
TEST(TuiRedrawPacerTest, EnforcesMinimumIntervalAfterCompletion) {
    TuiRedrawPacer pacer;
    ASSERT_TRUE(pacer.try_request_scheduled_redraw(1000, 50));
    const auto ticket = pacer.begin_frame(1001);
    pacer.complete_frame(ticket, 1020);

    EXPECT_FALSE(pacer.try_request_scheduled_redraw(1069, 50));
    EXPECT_TRUE(pacer.try_request_scheduled_redraw(1070, 50));
}

// 场景:终端帧异常昂贵,或防御性时间戳倒退。
// 期望:记录值封顶避免溢出;倒退按 0ms 处理。
TEST(TuiRedrawPacerTest, ClampsRecordedFrameLatency) {
    TuiRedrawPacer pacer;
    auto ticket = pacer.begin_frame(100);
    pacer.complete_frame(ticket, 100 + kMaxRecordedFrameLatencyMs + 1000);
    EXPECT_EQ(pacer.last_frame_latency_ms(), kMaxRecordedFrameLatencyMs);

    ticket = pacer.begin_frame(500);
    pacer.complete_frame(ticket, 499);
    EXPECT_EQ(pacer.last_frame_latency_ms(), 0);
}

}} // namespace acecode::tui
