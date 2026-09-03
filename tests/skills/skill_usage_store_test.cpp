#include "skills/skill_usage_store.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <thread>

namespace fs = std::filesystem;

namespace {

constexpr std::int64_t kDayMs = 24LL * 60 * 60 * 1000;

fs::path temp_state_file(const std::string& name) {
    fs::path tmp = fs::temp_directory_path() /
        ("acecode_skill_usage_" + name + ".json");
    std::error_code ec;
    fs::remove(tmp, ec);
    fs::remove(tmp.string() + ".tmp", ec);
    fs::remove(tmp.string() + ".lock", ec);
    return tmp;
}

void cleanup_state_file(const fs::path& path) {
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove(path.string() + ".tmp", ec);
    fs::remove(path.string() + ".lock", ec);
}

void write_state(const fs::path& path, const nlohmann::json& state) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
    ofs << state.dump(2);
    ASSERT_TRUE(ofs.good());
}

}  // namespace

TEST(SkillUsageStoreTest, ParseIso8601) {
    constexpr std::int64_t kExpected = 1785578400000LL;
    EXPECT_EQ(kExpected,
              acecode::parse_iso8601_to_epoch_ms("2026-08-01T10:00:00Z"));
    EXPECT_EQ(kExpected + 100,
              acecode::parse_iso8601_to_epoch_ms("2026-08-01T10:00:00.1Z"));
    EXPECT_EQ(kExpected + 120,
              acecode::parse_iso8601_to_epoch_ms("2026-08-01T10:00:00.12Z"));
    EXPECT_EQ(kExpected + 123,
              acecode::parse_iso8601_to_epoch_ms("2026-08-01T10:00:00.123Z"));
    // 无效输入返回 0
    EXPECT_EQ(0, acecode::parse_iso8601_to_epoch_ms("not-a-date"));
    EXPECT_EQ(0, acecode::parse_iso8601_to_epoch_ms(""));
    EXPECT_EQ(0, acecode::parse_iso8601_to_epoch_ms("2026-02-29T10:00:00Z"));
    EXPECT_EQ(0, acecode::parse_iso8601_to_epoch_ms("2026-13-01T10:00:00Z"));
    EXPECT_EQ(0, acecode::parse_iso8601_to_epoch_ms("2026-08-01T24:00:00Z"));
    EXPECT_EQ(0, acecode::parse_iso8601_to_epoch_ms("2026-08-01T10:00:60Z"));
    EXPECT_EQ(0, acecode::parse_iso8601_to_epoch_ms("2026-08-01T10:00:00"));
    EXPECT_EQ(0, acecode::parse_iso8601_to_epoch_ms("2026-08-01T10:00:00.1234Z"));
    EXPECT_EQ(0, acecode::parse_iso8601_to_epoch_ms("2026-08-01T10:00:00Zjunk"));
}

TEST(SkillUsageStoreTest, RecordCreatesEntry) {
    const fs::path tmp = temp_state_file("record");
    acecode::SkillUsageStore store(tmp.string());

    EXPECT_TRUE(store.record("pdf", "2026-08-01T10:00:00Z"));
    // 新记录应 active
    EXPECT_FALSE(store.is_dormant("pdf", 1785578400000LL, 30 * kDayMs));
    auto s = store.get_summary(1785578400000LL, 30 * kDayMs);
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].name, "pdf");
    EXPECT_EQ(s[0].use_count, 1u);
    EXPECT_FALSE(s[0].dormant);
    cleanup_state_file(tmp);
}

TEST(SkillUsageStoreTest, DormantAfterThreshold) {
    const fs::path tmp = temp_state_file("dormant");
    acecode::SkillUsageStore store(tmp.string());

    // 在 t=used_ms 使用,距今 40 天 > 30 天阈值
    const std::int64_t used_ms = 1785578400000LL;  // 2026-08-01T10:00:00Z
    const std::int64_t now_ms = used_ms + 40 * kDayMs;
    EXPECT_TRUE(store.record("xlsx", "2026-08-01T10:00:00Z"));
    EXPECT_TRUE(store.is_dormant("xlsx", now_ms, 30 * kDayMs));
    auto s = store.get_summary(now_ms, 30 * kDayMs);
    ASSERT_EQ(s.size(), 1u);
    EXPECT_TRUE(s[0].dormant);
    cleanup_state_file(tmp);
}

TEST(SkillUsageStoreTest, PinnedSkillNeverDormant) {
    const fs::path tmp = temp_state_file("pinned");
    acecode::SkillUsageStore store(tmp.string());

    EXPECT_TRUE(store.record("pinned_skill", "2026-06-01T10:00:00Z"));
    EXPECT_TRUE(store.set_pinned("pinned_skill", true));
    const std::int64_t now_ms = 1780308000000LL + 60 * kDayMs;
    EXPECT_FALSE(store.is_dormant("pinned_skill", now_ms, 30 * kDayMs));
    auto s = store.get_summary(now_ms, 30 * kDayMs);
    ASSERT_EQ(s.size(), 1u);
    EXPECT_FALSE(s[0].dormant);
    EXPECT_TRUE(s[0].pinned);
    cleanup_state_file(tmp);
}

TEST(SkillUsageStoreTest, IdleDaysZeroDisablesFeature) {
    const fs::path tmp = temp_state_file("zero");
    acecode::SkillUsageStore store(tmp.string());

    EXPECT_TRUE(store.record("skill", "2020-01-01T00:00:00Z"));
    EXPECT_FALSE(store.is_dormant("skill", 1785578400000LL, 0));
    auto s = store.get_summary(1785578400000LL, 0);
    ASSERT_EQ(s.size(), 1u);
    EXPECT_FALSE(s[0].dormant);
    cleanup_state_file(tmp);
}

TEST(SkillUsageStoreTest, IncrementUseCount) {
    const fs::path tmp = temp_state_file("incr");
    acecode::SkillUsageStore store(tmp.string());

    store.record("pdf", "2026-08-01T10:00:00Z");
    store.record("pdf", "2026-08-02T10:00:00Z");
    store.record("pdf", "2026-08-03T10:00:00Z");
    auto s = store.get_summary(1785578400000LL, 30 * kDayMs);
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].use_count, 3u);
    cleanup_state_file(tmp);
}

TEST(SkillUsageStoreTest, CorruptStateDegradesGracefully) {
    const fs::path tmp = temp_state_file("corrupt");
    {
        std::ofstream ofs(tmp);
        ofs << "{ this is not valid json";
    }
    acecode::SkillUsageStore store(tmp.string());

    // 损坏文件:record 仍成功(内部重置),不抛异常
    EXPECT_TRUE(store.record("pdf", "2026-08-01T10:00:00Z"));
    EXPECT_FALSE(store.is_dormant("pdf", 1785578400000LL, 30 * kDayMs));
    cleanup_state_file(tmp);
}

TEST(SkillUsageStoreTest, DamagedSchemaUsesSafeDefaultsAndNormalizesOnWrite) {
    const fs::path tmp = temp_state_file("damaged_schema");
    write_state(tmp, {
        {"version", 1},
        {"skills", {
            {"damaged", {
                {"lastUsedAt", 42},
                {"useCount", "many"},
                {"pinned", "yes"},
            }},
            {"scalar", "broken"},
        }},
    });

    acecode::SkillUsageStore store(tmp.string());
    EXPECT_FALSE(store.is_dormant("damaged", 1785578400000LL, 30 * kDayMs));
    auto summary = store.get_summary(1785578400000LL, 30 * kDayMs);
    ASSERT_EQ(summary.size(), 1u);
    EXPECT_EQ(summary[0].name, "damaged");
    EXPECT_EQ(summary[0].use_count, 0u);
    EXPECT_TRUE(summary[0].last_used_at.empty());
    EXPECT_FALSE(summary[0].pinned);

    EXPECT_TRUE(store.record("scalar", "2026-08-01T10:00:00Z"));
    EXPECT_TRUE(store.set_pinned("damaged", true));
    summary = store.get_summary(1785578400000LL, 30 * kDayMs);
    ASSERT_EQ(summary.size(), 2u);
    const auto scalar = std::find_if(
        summary.begin(), summary.end(),
        [](const auto& item) { return item.name == "scalar"; });
    ASSERT_NE(scalar, summary.end());
    EXPECT_EQ(scalar->use_count, 1u);
    const auto damaged = std::find_if(
        summary.begin(), summary.end(),
        [](const auto& item) { return item.name == "damaged"; });
    ASSERT_NE(damaged, summary.end());
    EXPECT_TRUE(damaged->pinned);
    cleanup_state_file(tmp);
}

TEST(SkillUsageStoreTest, UseCountSaturatesAtUint64Max) {
    const fs::path tmp = temp_state_file("saturates");
    write_state(tmp, {
        {"version", 1},
        {"skills", {
            {"pdf", {
                {"lastUsedAt", "2026-08-01T10:00:00Z"},
                {"useCount", std::numeric_limits<std::uint64_t>::max()},
                {"pinned", false},
            }},
        }},
    });

    acecode::SkillUsageStore store(tmp.string());
    EXPECT_TRUE(store.record("pdf", "2026-08-02T10:00:00Z"));
    const auto summary = store.get_summary(1785664800000LL, 30 * kDayMs);
    ASSERT_EQ(summary.size(), 1u);
    EXPECT_EQ(summary[0].use_count,
              std::numeric_limits<std::uint64_t>::max());
    cleanup_state_file(tmp);
}

TEST(SkillUsageStoreTest, SetPinnedNormalizesNegativeUseCountOnDisk) {
    const fs::path tmp = temp_state_file("negative_count");
    write_state(tmp, {
        {"version", 1},
        {"skills", {
            {"pdf", {
                {"lastUsedAt", "2026-08-01T10:00:00Z"},
                {"useCount", -7},
                {"pinned", false},
            }},
        }},
    });

    acecode::SkillUsageStore store(tmp.string());
    ASSERT_TRUE(store.set_pinned("pdf", true));

    std::ifstream input(tmp, std::ios::binary);
    ASSERT_TRUE(input.is_open());
    const auto persisted = nlohmann::json::parse(input);
    const auto& count = persisted["skills"]["pdf"]["useCount"];
    EXPECT_TRUE(count.is_number_unsigned());
    EXPECT_EQ(count.get<std::uint64_t>(), 0u);
    cleanup_state_file(tmp);
}

TEST(SkillUsageStoreTest, ConcurrentInstancesDoNotLoseUpdates) {
    const fs::path tmp = temp_state_file("concurrent_instances");
    acecode::SkillUsageStore first(tmp.string());
    acecode::SkillUsageStore second(tmp.string());
    constexpr int kRecordsPerInstance = 25;
    std::atomic<bool> start{false};
    std::atomic<bool> succeeded{true};
    auto record_many = [&](acecode::SkillUsageStore& store) {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < kRecordsPerInstance; ++i) {
            if (!store.record("pdf", "2026-08-01T10:00:00Z")) {
                succeeded.store(false, std::memory_order_release);
            }
        }
    };

    std::thread a(record_many, std::ref(first));
    std::thread b(record_many, std::ref(second));
    start.store(true, std::memory_order_release);
    a.join();
    b.join();

    EXPECT_TRUE(succeeded.load(std::memory_order_acquire));
    const auto summary = first.get_summary(1785578400000LL, 30 * kDayMs);
    ASSERT_EQ(summary.size(), 1u);
    EXPECT_EQ(summary[0].use_count, 2u * kRecordsPerInstance);
    cleanup_state_file(tmp);
}
