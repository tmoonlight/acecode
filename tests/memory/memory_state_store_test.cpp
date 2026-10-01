// 覆盖 src/domain/memory/memory_state_store.{hpp,cpp}(openspec unify-memory-system 2.4 / D5):
// 所有进程共用的记忆状态库 —— 租约互斥与过期接管、提炼进度只进不退与失败计数、
// 整合进度的待应用计划、墓碑 90 天过期、状态摘要。两个 MemoryStateStore 实例 =
// 两个进程各自的数据库连接。

#include <gtest/gtest.h>

#include "memory/memory_state_store.hpp"
#include "test_support/memory/memory_test_home.hpp"

#include <filesystem>

namespace {

using acecode::MemoryLeaseKind;
using acecode::MemoryStateStore;
using acecode_test::MemoryTestHome;

constexpr std::int64_t kNow = 1'800'000'000'000;  // 固定时钟,与真实时间无关
constexpr std::int64_t kDay = 24LL * 60 * 60 * 1000;

std::filesystem::path db_path(const MemoryTestHome& home) {
    return home.root() / "state" / "state.sqlite3";
}

} // namespace

// 场景:进程 A 拿到某会话的提炼租约,进程 B 同时来抢。
// 期望:B 拿不到;A 释放后 B 能拿到;A 续约只对自己的租约有效。
TEST(MemoryStateStoreTest, LeaseIsMutuallyExclusiveUntilReleased) {
    MemoryTestHome home("memory-state-lease");
    MemoryStateStore a(db_path(home));
    MemoryStateStore b(db_path(home));
    ASSERT_TRUE(a.try_acquire_lease(MemoryLeaseKind::Extraction, "s1", "owner-a", 60'000, kNow));
    EXPECT_FALSE(b.try_acquire_lease(MemoryLeaseKind::Extraction, "s1", "owner-b", 60'000, kNow));
    // 同一持有者重入视为续约成功。
    EXPECT_TRUE(a.try_acquire_lease(MemoryLeaseKind::Extraction, "s1", "owner-a", 60'000, kNow));
    EXPECT_TRUE(a.renew_lease(MemoryLeaseKind::Extraction, "s1", "owner-a", 60'000, kNow));
    EXPECT_FALSE(b.renew_lease(MemoryLeaseKind::Extraction, "s1", "owner-b", 60'000, kNow));
    // 不同类型的租约互不影响(整合租约按作用域)。
    EXPECT_TRUE(b.try_acquire_lease(MemoryLeaseKind::Consolidation, "global", "owner-b", 60'000, kNow));

    a.release_lease(MemoryLeaseKind::Extraction, "s1", "owner-a");
    EXPECT_TRUE(b.try_acquire_lease(MemoryLeaseKind::Extraction, "s1", "owner-b", 60'000, kNow));
}

// 场景:持有租约的进程崩溃(没有释放),租约到期后另一个进程来接手。
// 期望:到期前抢不到,到期后可以接管。
TEST(MemoryStateStoreTest, ExpiredLeaseCanBeTakenOver) {
    MemoryTestHome home("memory-state-expiry");
    MemoryStateStore a(db_path(home));
    MemoryStateStore b(db_path(home));
    ASSERT_TRUE(a.try_acquire_lease(MemoryLeaseKind::Consolidation, "global", "crashed", 1'000, kNow));
    EXPECT_FALSE(b.try_acquire_lease(MemoryLeaseKind::Consolidation, "global", "next", 1'000, kNow + 500));
    EXPECT_TRUE(b.try_acquire_lease(MemoryLeaseKind::Consolidation, "global", "next", 1'000, kNow + 1'000));
}

// 场景:同一范围的提炼崩溃后重放,较早的提交晚到。
// 期望:提炼位置只进不退;失败计数在会话活动标记不变时累加,标记变化后从 1 重新计。
TEST(MemoryStateStoreTest, ExtractionProgressNeverMovesBackwardAndCountsFailures) {
    MemoryTestHome home("memory-state-extraction");
    MemoryStateStore store(db_path(home));
    ASSERT_TRUE(store.commit_extraction("s1", "/p", 10, "10@t1", kNow));
    ASSERT_TRUE(store.commit_extraction("s1", "/p", 4, "4@t0", kNow));
    auto progress = store.extraction("s1");
    ASSERT_TRUE(progress.has_value());
    EXPECT_EQ(progress->next_message_index, 10);
    EXPECT_EQ(progress->seen_marker, "4@t0");

    store.record_extraction_failure("s1", "/p", "bad json", "12@t2", kNow);
    store.record_extraction_failure("s1", "/p", "bad json", "12@t2", kNow);
    EXPECT_EQ(store.extraction("s1")->attempts, 2);
    store.record_extraction_failure("s1", "/p", "timeout", "13@t3", kNow);
    progress = store.extraction("s1");
    EXPECT_EQ(progress->attempts, 1);
    EXPECT_EQ(progress->last_error, "timeout");
    EXPECT_EQ(progress->next_message_index, 10);

    ASSERT_TRUE(store.forget_extraction("s1"));
    EXPECT_FALSE(store.extraction("s1").has_value());
}

// 场景:整合计划校验通过后先存为待应用,应用成功再提交。
// 期望:待应用计划可读回;提交后清空待应用、记下计划 hash 与成功时间;失败记录
// 同一批次累加次数、换批次重新计数,并清掉待应用计划。
TEST(MemoryStateStoreTest, ConsolidationPendingPlanAndFailures) {
    MemoryTestHome home("memory-state-consolidation");
    MemoryStateStore store(db_path(home));
    ASSERT_TRUE(store.set_pending_plan("global", R"({"plan":"{}"})", "h1", kNow));
    auto progress = store.consolidation("global");
    ASSERT_TRUE(progress.has_value());
    EXPECT_EQ(progress->pending_plan_hash, "h1");

    ASSERT_TRUE(store.commit_consolidation("global", "h1", kNow + 5));
    progress = store.consolidation("global");
    EXPECT_TRUE(progress->pending_plan.empty());
    EXPECT_EQ(progress->last_plan_hash, "h1");
    EXPECT_EQ(progress->last_success_ms, kNow + 5);

    store.record_consolidation_failure("global", "invalid plan", "batch-a", kNow);
    store.record_consolidation_failure("global", "invalid plan", "batch-a", kNow);
    EXPECT_EQ(store.consolidation("global")->attempts, 2);
    store.record_consolidation_failure("global", "invalid plan", "batch-b", kNow);
    EXPECT_EQ(store.consolidation("global")->attempts, 1);
}

// 场景:用户删除条目后记墓碑;按名字或规范化标题(忽略大小写、空白)查询;91 天后。
// 期望:90 天内命中,过期后不命中且可被清理;墓碑按作用域隔离。
TEST(MemoryStateStoreTest, TombstonesExpireAfterNinetyDays) {
    MemoryTestHome home("memory-state-tombstone");
    MemoryStateStore store(db_path(home));
    ASSERT_TRUE(store.add_tombstone("workspace:abc", "old_rule", "Use  Tabs ", kNow));
    EXPECT_TRUE(store.is_tombstoned("workspace:abc", "OLD_RULE", "", kNow + kDay));
    EXPECT_TRUE(store.is_tombstoned("workspace:abc", "other", "use tabs", kNow + kDay));
    EXPECT_FALSE(store.is_tombstoned("global", "old_rule", "", kNow + kDay));
    EXPECT_FALSE(store.is_tombstoned("workspace:abc", "old_rule", "", kNow + 91 * kDay));
    EXPECT_EQ(store.tombstones("workspace:abc", kNow).size(), 1u);
    EXPECT_EQ(store.purge_expired_tombstones(kNow + 91 * kDay), 1);
    EXPECT_TRUE(store.tombstones("workspace:abc", kNow).empty());
}

// 场景:写入最近提炼时间与最近错误。期望:能读回值与时间。
TEST(MemoryStateStoreTest, StatusRoundTrip) {
    MemoryTestHome home("memory-state-status");
    MemoryStateStore store(db_path(home));
    ASSERT_TRUE(store.set_status("last_error", "plan rejected", kNow));
    const auto status = store.status("last_error");
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->first, "plan rejected");
    EXPECT_EQ(status->second, kNow);
    EXPECT_FALSE(store.status("missing").has_value());
}

// 场景:跨进程写锁(BEGIN IMMEDIATE)嵌套使用,以及没有状态库时的降级。
// 期望:同一线程嵌套构造复用外层事务;提交后另一个连接能立刻拿到锁;
// store 为空时 acquired()=false 而不是崩溃。
TEST(MemoryStateStoreTest, WriteLockNestsAndReleases) {
    MemoryTestHome home("memory-state-writelock");
    MemoryStateStore a(db_path(home));
    MemoryStateStore b(db_path(home));
    {
        acecode::MemoryWriteLock outer(&a);
        ASSERT_TRUE(outer.acquired()) << outer.error();
        {
            acecode::MemoryWriteLock inner(&a);
            EXPECT_TRUE(inner.acquired());
            EXPECT_TRUE(inner.commit());
        }
        EXPECT_TRUE(outer.commit());
    }
    acecode::MemoryWriteLock other(&b);
    EXPECT_TRUE(other.acquired()) << other.error();
    EXPECT_TRUE(other.commit());

    acecode::MemoryWriteLock none(nullptr);
    EXPECT_FALSE(none.acquired());
}

// 场景:标题规范化。期望:去首尾空白、ASCII 小写、连续空白折成一个空格,中文不变。
TEST(MemoryStateStoreTest, NormalizeTitle) {
    EXPECT_EQ(acecode::normalize_memory_title("  Hello   World \n"), "hello world");
    EXPECT_EQ(acecode::normalize_memory_title(u8"构建 步骤"), u8"构建 步骤");
}
