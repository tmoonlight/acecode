#include "session/session_load_metrics.hpp"
#include <gtest/gtest.h>

TEST(SessionLoadMetrics, NestedScopesCountReadsAndUseWarningThreshold) {
    using namespace acecode;
    auto now = std::chrono::steady_clock::time_point{};
    std::vector<std::pair<LogLevel, nlohmann::json>> logged;
    const auto clock = [&] { return now; };
    const auto sink = [&](LogLevel level, const nlohmann::json& payload) {
        logged.emplace_back(level, payload);
    };
    {
        SessionLoadTimer request("history", "synthetic", clock, sink);
        session_read_metrics().bytes += 10;
        {
            SessionLoadTimer nested("load", "synthetic", clock, sink);
            session_read_metrics().bytes += 20;
            session_read_metrics().files += 1;
            session_read_metrics().records += 2;
            now += std::chrono::milliseconds(500);
        }
        now += std::chrono::milliseconds(1);
    }
    ASSERT_EQ(logged.size(), 2u);
    EXPECT_EQ(logged[0].first, LogLevel::Dbg);
    EXPECT_EQ(logged[0].second["read_bytes"], 20);
    EXPECT_EQ(logged[1].first, LogLevel::Warn);
    EXPECT_EQ(logged[1].second["elapsed_ms"], 501);
    EXPECT_EQ(logged[1].second["read_bytes"], 30);
    EXPECT_EQ(logged[1].second["opened_files"], 1);
    EXPECT_EQ(logged[1].second["parsed_records"], 2);
    EXPECT_FALSE(logged[1].second.contains("content"));
}

TEST(SessionLoadMetrics, FailingSinkCannotInterruptRequest) {
    EXPECT_NO_THROW({
        acecode::SessionLoadTimer timer("history", "", [] {
            return std::chrono::steady_clock::now();
        }, [](acecode::LogLevel, const nlohmann::json&) { throw 1; });
    });
}
