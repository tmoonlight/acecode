#include <gtest/gtest.h>

#include "session/session_history_recovery.hpp"
#include "session/session_serializer.hpp"

#include <string>
#include <vector>

namespace {

acecode::ChatMessage text_message(const std::string& role,
                                  const std::string& content) {
    acecode::ChatMessage msg;
    msg.role = role;
    msg.content = content;
    return msg;
}

nlohmann::json tool_call(const std::string& id,
                         const std::string& name = "file_read") {
    return nlohmann::json{
        {"id", id},
        {"type", "function"},
        {"function", {
            {"name", name},
            {"arguments", R"({"path":"README.md"})"},
        }},
    };
}

acecode::ChatMessage assistant_with_calls(
    std::initializer_list<nlohmann::json> calls,
    const std::string& content = {}) {
    auto msg = text_message("assistant", content);
    msg.tool_calls = nlohmann::json::array();
    for (const auto& call : calls) msg.tool_calls.push_back(call);
    return msg;
}

acecode::ChatMessage tool_result(const std::string& id,
                                 const std::string& content) {
    auto msg = text_message("tool", content);
    msg.tool_call_id = id;
    return msg;
}

} // namespace

TEST(SessionHistoryRecovery, LeavesCleanHistoryUnchanged) {
    const std::vector<acecode::ChatMessage> input = {
        text_message("system", "rules"),
        text_message("user", "inspect"),
        assistant_with_calls({tool_call("call-1")}, "reading"),
        tool_result("call-1", "contents"),
        text_message("assistant", "done"),
    };

    const auto recovered = acecode::recover_provider_history(input);

    EXPECT_FALSE(recovered.stats.changed());
    ASSERT_EQ(recovered.messages.size(), input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        EXPECT_EQ(acecode::serialize_message(recovered.messages[i]),
                  acecode::serialize_message(input[i]));
    }
}

TEST(SessionHistoryRecovery, SynthesizesOutcomeUnknownResultBeforeNextTurn) {
    const auto recovered = acecode::recover_provider_history({
        text_message("user", "change it"),
        assistant_with_calls({tool_call("call-1", "file_write")}),
        text_message("user", "continue"),
    });

    ASSERT_EQ(recovered.messages.size(), 4u);
    EXPECT_EQ(recovered.messages[1].role, "assistant");
    EXPECT_EQ(recovered.messages[2].role, "tool");
    EXPECT_EQ(recovered.messages[2].tool_call_id, "call-1");
    EXPECT_NE(recovered.messages[2].content.find("outcome is unknown"),
              std::string::npos);
    EXPECT_NE(recovered.messages[2].content.find("do not assume success or failure"),
              std::string::npos);
    ASSERT_TRUE(recovered.messages[2].metadata.contains("session_recovery"));
    EXPECT_EQ(recovered.messages[2].metadata["session_recovery"]["outcome"],
              "unknown");
    EXPECT_EQ(recovered.messages[3].role, "user");
    EXPECT_EQ(recovered.stats.synthesized_tool_results, 1u);
}

TEST(SessionHistoryRecovery, PreservesRealResultsAndFillsOnlyMissingOnes) {
    const auto recovered = acecode::recover_provider_history({
        assistant_with_calls({tool_call("call-a"), tool_call("call-b")}),
        tool_result("call-a", "real output"),
        text_message("assistant", "next"),
    });

    ASSERT_EQ(recovered.messages.size(), 4u);
    EXPECT_EQ(recovered.messages[1].tool_call_id, "call-a");
    EXPECT_EQ(recovered.messages[1].content, "real output");
    EXPECT_EQ(recovered.messages[2].tool_call_id, "call-b");
    EXPECT_NE(recovered.messages[2].content.find("outcome is unknown"),
              std::string::npos);
    EXPECT_EQ(recovered.stats.synthesized_tool_results, 1u);
}

TEST(SessionHistoryRecovery, SystemContextDoesNotHideARealFollowingResult) {
    const auto recovered = acecode::recover_provider_history({
        assistant_with_calls({tool_call("call-1")}),
        text_message("system", "late context"),
        tool_result("call-1", "real output"),
    });

    ASSERT_EQ(recovered.messages.size(), 3u);
    EXPECT_EQ(recovered.messages[0].role, "assistant");
    EXPECT_EQ(recovered.messages[1].role, "system");
    EXPECT_EQ(recovered.messages[2].role, "tool");
    EXPECT_EQ(recovered.messages[2].content, "real output");
    EXPECT_FALSE(recovered.stats.changed());
}

TEST(SessionHistoryRecovery, DropsStandaloneUnexpectedAndDuplicateResults) {
    const auto recovered = acecode::recover_provider_history({
        tool_result("never-called", "standalone"),
        assistant_with_calls({tool_call("call-a"), tool_call("call-b")}),
        tool_result("wrong-id", "unexpected"),
        tool_result("call-a", "first"),
        tool_result("call-a", "duplicate"),
        tool_result("call-b", "second"),
    });

    ASSERT_EQ(recovered.messages.size(), 3u);
    EXPECT_EQ(recovered.messages[0].role, "assistant");
    EXPECT_EQ(recovered.messages[1].content, "first");
    EXPECT_EQ(recovered.messages[2].content, "second");
    EXPECT_EQ(recovered.stats.standalone_tool_results, 1u);
    EXPECT_EQ(recovered.stats.unexpected_tool_results, 1u);
    EXPECT_EQ(recovered.stats.duplicate_tool_results, 1u);
    EXPECT_EQ(recovered.stats.synthesized_tool_results, 0u);
}

TEST(SessionHistoryRecovery, RemovesMalformedAndDuplicateCallsWithoutLosingText) {
    nlohmann::json missing_id = tool_call("", "bash");
    nlohmann::json missing_name = tool_call("call-bad");
    missing_name["function"].erase("name");

    const auto recovered = acecode::recover_provider_history({
        assistant_with_calls({
            missing_id,
            missing_name,
            tool_call("call-good"),
            tool_call("call-good"),
        }, "I can still explain this"),
        tool_result("call-good", "ok"),
    });

    ASSERT_EQ(recovered.messages.size(), 2u);
    EXPECT_EQ(recovered.messages[0].content, "I can still explain this");
    ASSERT_TRUE(recovered.messages[0].tool_calls.is_array());
    ASSERT_EQ(recovered.messages[0].tool_calls.size(), 1u);
    EXPECT_EQ(recovered.messages[0].tool_calls[0]["id"], "call-good");
    EXPECT_EQ(recovered.stats.malformed_tool_calls, 2u);
    EXPECT_EQ(recovered.stats.duplicate_tool_calls, 1u);
}

TEST(SessionHistoryRecovery, DropsEmptyAssistantWhenAllCallsAreMalformed) {
    const auto recovered = acecode::recover_provider_history({
        assistant_with_calls({nlohmann::json{{"type", "function"}}}),
        text_message("user", "resume"),
    });

    ASSERT_EQ(recovered.messages.size(), 1u);
    EXPECT_EQ(recovered.messages.front().role, "user");
    EXPECT_EQ(recovered.stats.malformed_tool_calls, 1u);
    EXPECT_EQ(recovered.stats.empty_assistant_messages, 1u);
}
