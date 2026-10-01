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

// 同一 ID 可以来自多个模型步骤；真实输出及调用身份必须逐步保留。
TEST(SessionHistoryRecovery, PreservesReusedIdsAcrossStepsAndUserTurns) {
    auto vision_result = tool_result("call_0", "screen description");
    vision_result.metadata = {{"attachment_id", "test-attachment"}};
    vision_result.content_parts = nlohmann::json::array({
        {{"type", "text"}, {"text", "screen description"}},
    });
    const std::vector<acecode::ChatMessage> input = {
        text_message("user", "describe the screen"),
        assistant_with_calls({tool_call("call_0", "bash")}, "capture"),
        tool_result("call_0", "screenshot saved"),
        assistant_with_calls({tool_call("call_0", "vision_analyze")}, "inspect"),
        vision_result,
        text_message("user", "continue"),
        assistant_with_calls({tool_call("call_0", "vision_analyze")}),
        tool_result("call_0", "new description"),
    };
    std::vector<std::string> original;
    for (const auto& msg : input) original.push_back(acecode::serialize_message(msg));

    const auto recovered = acecode::recover_provider_history(input);
    ASSERT_EQ(recovered.messages.size(), input.size());
    const auto second_id = recovered.messages[3].tool_calls.at(0).at("id");
    const auto third_id = recovered.messages[6].tool_calls.at(0).at("id");
    EXPECT_EQ(recovered.messages[1].tool_calls.at(0).at("id"), "call_0");
    EXPECT_NE(second_id, "call_0");
    EXPECT_NE(third_id, "call_0");
    EXPECT_NE(second_id, third_id);
    EXPECT_EQ(recovered.messages[4].tool_call_id, second_id);
    EXPECT_EQ(recovered.messages[7].tool_call_id, third_id);
    EXPECT_EQ(recovered.messages.back().role, "tool");
    EXPECT_EQ(recovered.stats.duplicate_tool_calls, 2u);
    EXPECT_EQ(recovered.stats.duplicate_tool_results, 0u);
    EXPECT_EQ(recovered.stats.synthesized_tool_results, 0u);

    // 只有投影中的 ID 改变，输入、调用参数、结果附件和元数据均保持原样。
    for (std::size_t i = 0; i < input.size(); ++i) {
        EXPECT_EQ(acecode::serialize_message(input[i]), original[i]);
        auto restored = recovered.messages[i];
        restored.tool_calls = input[i].tool_calls;
        restored.tool_call_id = input[i].tool_call_id;
        EXPECT_EQ(acecode::serialize_message(restored), original[i]);
    }

    const auto repeated = acecode::recover_provider_history(input);
    const auto projected = acecode::recover_provider_history(recovered.messages);
    EXPECT_FALSE(projected.stats.changed());
    ASSERT_EQ(projected.messages.size(), recovered.messages.size());
    ASSERT_EQ(repeated.messages.size(), recovered.messages.size());
    for (std::size_t i = 0; i < recovered.messages.size(); ++i) {
        EXPECT_EQ(acecode::serialize_message(repeated.messages[i]),
                  acecode::serialize_message(recovered.messages[i]));
        EXPECT_EQ(acecode::serialize_message(projected.messages[i]),
                  acecode::serialize_message(recovered.messages[i]));
    }
}

TEST(SessionHistoryRecovery, PairsReusedParallelCallsAcrossSystemContext) {
    const auto recovered = acecode::recover_provider_history({
        assistant_with_calls({tool_call("a"), tool_call("b")}),
        tool_result("a", "first a"),
        tool_result("b", "first b"),
        assistant_with_calls({tool_call("a"), tool_call("b"), tool_call("a")}),
        text_message("system", "late context"),
        tool_result("b", "second b"),
        tool_result("wrong", "unexpected"),
        tool_result("b", "duplicate b"),
        tool_result("a", "second a"),
    });
    ASSERT_EQ(recovered.messages.size(), 7u);
    const auto& calls = recovered.messages[3].tool_calls;
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_NE(calls[0]["id"], calls[1]["id"]);
    EXPECT_EQ(recovered.messages[4].role, "system");
    EXPECT_EQ(recovered.messages[5].tool_call_id, calls[1]["id"]);
    EXPECT_EQ(recovered.messages[5].content, "second b");
    EXPECT_EQ(recovered.messages[6].tool_call_id, calls[0]["id"]);
    EXPECT_EQ(recovered.messages[6].content, "second a");
    EXPECT_EQ(recovered.stats.duplicate_tool_calls, 3u);
    EXPECT_EQ(recovered.stats.duplicate_tool_results, 1u);
    EXPECT_EQ(recovered.stats.unexpected_tool_results, 1u);
    EXPECT_EQ(recovered.stats.synthesized_tool_results, 0u);
}

TEST(SessionHistoryRecovery, ReservesFutureCallAndUnmatchedResultIds) {
    const auto recovered = acecode::recover_provider_history({
        assistant_with_calls({tool_call("call_0")}),
        tool_result("call_0", "first"),
        assistant_with_calls({tool_call("call_0")}),
        tool_result("call_ace_recovered_1", "unmatched"),
        tool_result("call_0", "second"),
        assistant_with_calls({tool_call("call_ace_recovered_2")}),
        tool_result("call_ace_recovered_2", "third"),
    });
    ASSERT_EQ(recovered.messages.size(), 6u);
    const auto& second_id = recovered.messages[2].tool_calls.at(0).at("id");
    EXPECT_NE(second_id, "call_0");
    EXPECT_NE(second_id, "call_ace_recovered_1");
    EXPECT_NE(second_id, "call_ace_recovered_2");
    EXPECT_EQ(recovered.messages[3].tool_call_id, second_id);
    EXPECT_EQ(recovered.messages[3].content, "second");
    EXPECT_EQ(recovered.messages[4].tool_calls.at(0).at("id"), "call_ace_recovered_2");
    EXPECT_EQ(recovered.messages[5].tool_call_id, "call_ace_recovered_2");
    EXPECT_EQ(recovered.messages[5].content, "third");
    EXPECT_EQ(recovered.stats.unexpected_tool_results, 1u);
}

TEST(SessionHistoryRecovery, MissingReusedCallResultGetsItsOwnPlaceholder) {
    const auto recovered = acecode::recover_provider_history({
        assistant_with_calls({tool_call("call_0")}),
        tool_result("call_0", "first result"),
        assistant_with_calls({tool_call("call_0")}),
        text_message("user", "continue"),
    });
    ASSERT_EQ(recovered.messages.size(), 5u);
    const auto& second_id = recovered.messages[2].tool_calls.at(0).at("id");
    EXPECT_NE(second_id, "call_0");
    EXPECT_EQ(recovered.messages[1].content, "first result");
    EXPECT_EQ(recovered.messages[3].tool_call_id, second_id);
    EXPECT_EQ(recovered.messages[3].metadata["session_recovery"]["outcome"], "unknown");
    EXPECT_EQ(recovered.stats.synthesized_tool_results, 1u);
}

TEST(SessionHistoryRecovery, ReuseAfterInterruptedCallKeepsRealResultSeparate) {
    const auto recovered = acecode::recover_provider_history({
        assistant_with_calls({tool_call("call_0")}),
        assistant_with_calls({tool_call("call_0")}),
        tool_result("call_0", "second result"),
    });
    ASSERT_EQ(recovered.messages.size(), 4u);
    EXPECT_EQ(recovered.messages[1].tool_call_id, "call_0");
    EXPECT_EQ(recovered.messages[1].metadata["session_recovery"]["outcome"], "unknown");
    EXPECT_NE(recovered.messages[2].tool_calls.at(0).at("id"), "call_0");
    EXPECT_EQ(recovered.messages[3].tool_call_id, recovered.messages[2].tool_calls[0]["id"]);
    EXPECT_EQ(recovered.messages[3].content, "second result");
    EXPECT_EQ(recovered.stats.synthesized_tool_results, 1u);
}

TEST(SessionHistoryRecovery, RemapsLegacySingleToolCallObjects) {
    auto assistant = text_message("assistant", "");
    assistant.tool_calls = tool_call("call_0");
    const auto recovered = acecode::recover_provider_history({
        assistant, tool_result("call_0", "first"),
        assistant, tool_result("call_0", "second"),
    });
    ASSERT_EQ(recovered.messages.size(), 4u);
    ASSERT_TRUE(recovered.messages[2].tool_calls.is_array());
    EXPECT_NE(recovered.messages[2].tool_calls.at(0).at("id"), "call_0");
    EXPECT_EQ(recovered.messages[3].tool_call_id, recovered.messages[2].tool_calls[0]["id"]);
    EXPECT_EQ(recovered.messages[3].content, "second");
}
