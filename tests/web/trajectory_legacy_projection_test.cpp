#include "web/message_payload.hpp"
#include "web/trajectory_legacy_projection.hpp"
#include "session/turn_timing.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace {

acecode::ChatMessage user_message() {
    acecode::ChatMessage message;
    message.role = "user";
    message.content = "inspect old session";
    message.uuid = "turn-1";
    message.timestamp = "2026-08-15T01:02:03Z";
    return message;
}

std::vector<acecode::ChatMessage> legacy_messages() {
    std::vector<acecode::ChatMessage> messages;
    messages.push_back(user_message());

    acecode::ChatMessage assistant;
    assistant.role = "assistant";
    assistant.content = "I will inspect it";
    assistant.reasoning_content = "legacy reasoning";
    messages.push_back(assistant);

    acecode::ChatMessage tool_call;
    tool_call.role = "assistant";
    tool_call.tool_calls = nlohmann::json::array({
        {{"id", "call-1"},
         {"type", "function"},
         {"function", {{"name", "file_read"},
                       {"arguments", "{\"file_path\":\"old.txt\"}"}}}},
    });
    messages.push_back(tool_call);

    acecode::ChatMessage tool_result;
    tool_result.role = "tool";
    tool_result.tool_call_id = "call-1";
    tool_result.content = "old file contents";
    messages.push_back(tool_result);

    acecode::TurnTimingRecord timing;
    timing.user_message_uuid = "turn-1";
    timing.started_at_ms = 1000;
    timing.completed_at_ms = 2500;
    timing.duration_ms = 1500;
    timing.status = "completed";
    messages.push_back(acecode::make_turn_timing_message(
        timing, "2026-08-15T01:02:05Z"));
    return messages;
}

TEST(TrajectoryLegacyProjection, PreservesOnlyKnownFactsAndMissingCapabilities) {
    const auto page = acecode::web::project_legacy_trajectory(
        legacy_messages(), {}, 0, 100);
    ASSERT_EQ(page.records.size(), 5u);
    EXPECT_FALSE(page.has_more);
    EXPECT_EQ(page.total, 5u);
    EXPECT_NE(std::find(page.missing_capabilities.begin(),
                        page.missing_capabilities.end(), "ttft"),
              page.missing_capabilities.end());

    EXPECT_EQ(page.records[0].value("type", std::string{}),
              "legacy_user_message");
    EXPECT_EQ(page.records[0]["timestamp_ms"], 1000);
    EXPECT_EQ(page.records[1].value("type", std::string{}),
              "legacy_model_response");
    EXPECT_TRUE(page.records[1]["timestamp_ms"].is_null());
    EXPECT_EQ(page.records[1]["payload"].value(
                  "reasoning_content", std::string{}),
              "legacy reasoning");
    EXPECT_EQ(page.records[3].value("type", std::string{}),
              "legacy_tool_result");
    EXPECT_EQ(page.records[3]["payload"].value("output", std::string{}),
              "old file contents");
    EXPECT_EQ(page.records[4]["payload"].value("duration_ms", 0), 1500);
}

TEST(TrajectoryLegacyProjection, PaginatesByLegacyOffset) {
    const auto first = acecode::web::project_legacy_trajectory(
        legacy_messages(), {}, 0, 2);
    ASSERT_EQ(first.records.size(), 2u);
    EXPECT_TRUE(first.has_more);
    EXPECT_EQ(first.next_after, 2u);

    const auto second = acecode::web::project_legacy_trajectory(
        legacy_messages(), {}, first.next_after, 2);
    ASSERT_EQ(second.records.size(), 2u);
    EXPECT_EQ(second.records[0].value("legacy_index", 0u), 2u);
    EXPECT_TRUE(second.has_more);
}

TEST(TrajectoryLegacyProjection, MixedSourceDeduplicatesStableFacts) {
    const auto messages = legacy_messages();
    acecode::SessionTrajectoryRecord assistant;
    assistant.sequence = 1;
    assistant.timestamp_ms = 3000;
    assistant.type = "model_response";
    assistant.payload = {
        {"message_id", acecode::web::compute_message_id(messages[1])},
        {"tool_calls", nlohmann::json::array()},
    };
    acecode::SessionTrajectoryRecord tool_end;
    tool_end.sequence = 2;
    tool_end.timestamp_ms = 3100;
    tool_end.type = "tool_end";
    tool_end.payload = {{"tool_call_id", "call-1"}};
    acecode::SessionTrajectoryRecord turn_end;
    turn_end.sequence = 3;
    turn_end.timestamp_ms = 3200;
    turn_end.type = "turn_end";
    turn_end.payload = {{"turn_id", "turn-1"}};

    const auto page = acecode::web::project_legacy_trajectory(
        messages, {assistant, tool_end, turn_end}, 0, 100);
    ASSERT_EQ(page.records.size(), 1u);
    EXPECT_EQ(page.records[0].value("type", std::string{}),
              "legacy_user_message");
}

} // namespace
