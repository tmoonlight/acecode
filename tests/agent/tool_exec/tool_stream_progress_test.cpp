#include <gtest/gtest.h>

#include "agent/tool_exec/tool_stream_progress.hpp"

#include <chrono>
#include <string>
#include <vector>

using namespace std::chrono_literals;

TEST(ToolStreamProgress, SnapshotsKeepByteLineAndCarriageReturnSemantics) {
    acecode::agent::ToolStreamProgress progress;
    const auto start = acecode::agent::ToolStreamProgress::Clock::time_point{} + 1s;
    const auto first = progress.append("old\rnew\npart", start);
    EXPECT_TRUE(first.should_emit);
    EXPECT_EQ(first.total_bytes, 12u);
    EXPECT_EQ(first.total_lines, 1);
    EXPECT_EQ(first.tail_lines, (std::vector<std::string>{"new"}));
    EXPECT_EQ(first.current_partial, "part");
    const auto second = progress.append("ial\n", start + 499ms);
    EXPECT_FALSE(second.should_emit);
    EXPECT_EQ(second.total_bytes, 16u);
    EXPECT_EQ(second.tail_lines, (std::vector<std::string>{"new", "partial"}));
    EXPECT_EQ(first.current_partial, "part"); // The first snapshot is independent.
    EXPECT_TRUE(progress.append("next", start + 500ms).should_emit);
}

TEST(ToolStreamProgress, KeepsOnlyTheLatestFiveCompletedLines) {
    acecode::agent::ToolStreamProgress progress;
    const auto snapshot = progress.append("0\n1\n2\n3\n4\n5\n6\n");
    EXPECT_EQ(snapshot.total_lines, 7);
    EXPECT_EQ(snapshot.tail_lines, (std::vector<std::string>{"2", "3", "4", "5", "6"}));
}
