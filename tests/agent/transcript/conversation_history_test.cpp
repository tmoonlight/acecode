#include <gtest/gtest.h>

#include "agent/transcript/conversation_history.hpp"
#include "session/session_client.hpp"

#include <atomic>
#include <string>

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
