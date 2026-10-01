#include <gtest/gtest.h>

#include "agent/transcript/conversation_history.hpp"
#include "session/session_client.hpp"
#include "session/tool_result_storage.hpp"

#include <atomic>
#include <string>
#include <set>

namespace {
acecode::SessionEvent event(acecode::SessionEventKind kind, nlohmann::json payload) {
    acecode::SessionEvent value;
    value.kind = kind;
    value.payload = std::move(payload);
    return value;
}
} // namespace

TEST(ConversationHistory, RestoreAndReplaceKeepCanonicalIdentityAndMetadata) {
    std::atomic<bool> busy{false};
    acecode::agent::ConversationHistory history(busy);
    acecode::ChatMessage user;
    user.role = "user";
    user.content = "expanded prompt";
    user.uuid = "persisted-user-id";
    user.timestamp = "2026-09-28T00:00:00Z";
    user.metadata = {{"display_text", "original prompt"}, {"turn_steer", true}};
    history.restore(user, true);
    auto previous = history.view();
    history.on_worker([&](acecode::agent::ConversationHistory& writable) {
        acecode::ChatMessage assistant;
        assistant.role = "assistant";
        assistant.content = "summary";
        writable.replace({user, assistant});
    }, true);
    ASSERT_EQ(history.view().size(), 2u);
    EXPECT_EQ(history.view()[0].uuid, "persisted-user-id");
    EXPECT_EQ(history.view()[0].timestamp, user.timestamp);
    EXPECT_EQ(history.view()[0].metadata, user.metadata);
    EXPECT_EQ(previous.size(), 1u);
    history.clear_idle(true);
    EXPECT_TRUE(history.view().empty());
}

TEST(ConversationHistory, TransientOutputBlocksRetryButBookkeepingDoesNotOverrideTail) {
    std::atomic<bool> busy{false};
    acecode::agent::ConversationHistory history(busy);
    history.observe_transcript(event(acecode::SessionEventKind::Token, {{"text", ""}}));
    EXPECT_FALSE(history.retry_blocked());
    history.observe_transcript(event(acecode::SessionEventKind::Token, {{"text", "partial"}}));
    EXPECT_TRUE(history.retry_blocked());
    history.observe_transcript(event(acecode::SessionEventKind::Message,
        {{"role", "user"}, {"metadata", {{"hidden_goal_context", true}}}}));
    EXPECT_TRUE(history.retry_blocked());
    history.observe_transcript(event(acecode::SessionEventKind::TranscriptReplace, nlohmann::json::object()));
    EXPECT_FALSE(history.retry_blocked());
    history.observe_transcript(event(acecode::SessionEventKind::ToolStart, nlohmann::json::object()));
    EXPECT_TRUE(history.retry_blocked());
    history.observe_transcript(event(acecode::SessionEventKind::Message,
        {{"role", "system"}, {"metadata", {{"transcript_only", true}, {"user_aborted", true}}}}));
    EXPECT_FALSE(history.retry_blocked());
    EXPECT_TRUE(history.view().empty());
}

TEST(ConversationHistory, ExecutionIdsSurviveBatchesCompactionAndClear) {
    std::atomic<bool> busy{false};
    acecode::agent::ConversationHistory history(busy);
    std::vector<acecode::ToolCall> batch = {
        {"call_0", "first", "{}"}, {"call_0", "second", "{}"},
        {"", "third", "{}"}, {"call_1", "fourth", "{}"},
    };
    history.prepare_tool_calls(batch);
    EXPECT_EQ(batch[0].id, "call_0");
    EXPECT_EQ(batch[3].id, "call_1");
    std::set<std::string> ids;
    for (const auto& call : batch) {
        EXPECT_FALSE(call.id.empty());
        EXPECT_LE(call.id.size(), 64u);
        EXPECT_TRUE(ids.insert(call.id).second);
    }
    EXPECT_EQ(batch[1].function_name, "second");
    EXPECT_EQ(batch[1].function_arguments, "{}");
    history.replace({});
    std::vector<acecode::ToolCall> after_compact = {{"call_0", "probe", "{}"}};
    history.prepare_tool_calls(after_compact);
    EXPECT_TRUE(ids.insert(after_compact[0].id).second);
    history.clear_idle(true);
    std::vector<acecode::ToolCall> after_clear = {{"call_0", "probe", "{}"}};
    history.prepare_tool_calls(after_clear);
    EXPECT_TRUE(ids.insert(after_clear[0].id).second);
}

TEST(ConversationHistory, RestoredCallsResultsAndReplacementRecordsReserveIds) {
    std::atomic<bool> busy{false};
    acecode::agent::ConversationHistory history(busy);
    acecode::ChatMessage assistant;
    assistant.role = "assistant";
    assistant.tool_calls = nlohmann::json::array({
        {{"id", "call_assistant"}, {"type", "function"}},
        {{"id", 42}}, nullptr,
    });
    history.restore(assistant, true);
    acecode::ChatMessage result;
    result.role = "tool";
    result.tool_call_id = "call_result";
    history.replace({result, acecode::encode_content_replacement_message({
        {"call_record", "preview"},
    })});
    std::vector<acecode::ToolCall> calls = {
        {"call_assistant", "probe", "{}"}, {"call_result", "probe", "{}"},
        {"call_record", "probe", "{}"}, {"new_id", "probe", "{}"},
    };
    const auto original = calls;
    history.prepare_tool_calls(calls);
    for (std::size_t i = 0; i < 3; ++i) EXPECT_NE(calls[i].id, original[i].id);
    EXPECT_EQ(calls[3].id, "new_id");
    EXPECT_EQ(history.view()[0].tool_call_id, "call_result");
}
