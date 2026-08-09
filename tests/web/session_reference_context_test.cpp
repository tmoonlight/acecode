#include "web/session_reference_context.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace acecode::web {
namespace {

using nlohmann::json;

ChatMessage message(std::string role, std::string content) {
    ChatMessage value;
    value.role = std::move(role);
    value.content = std::move(content);
    return value;
}

TEST(SessionReferenceContext, ParsesAndDeduplicatesBoundedDescriptors) {
    const json reference = {
        {"session_id", "session-1"},
        {"workspace_hash", "workspace-a"},
        {"no_workspace", false},
        {"title", "Source task"},
        {"workspace_name", "ACECode"},
    };
    const auto parsed = parse_session_reference_descriptors(
        json::array({reference, reference}));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    ASSERT_EQ(parsed.references.size(), 1u);
    EXPECT_EQ(parsed.references[0].session_id, "session-1");
    EXPECT_EQ(parsed.references[0].workspace_hash, "workspace-a");
    EXPECT_EQ(parsed.references[0].title, "Source task");
}

TEST(SessionReferenceContext, RejectsUnsafeIdsAndTooManyReferences) {
    const json unsafe = json::array({json{
        {"session_id", "../secret"},
        {"workspace_hash", "workspace-a"},
    }});
    auto parsed = parse_session_reference_descriptors(unsafe);
    EXPECT_FALSE(parsed.ok);
    EXPECT_EQ(parsed.error, "invalid referenced session id");

    json many = json::array();
    for (std::size_t i = 0; i <= kMaxSessionReferences; ++i) {
        many.push_back(json{
            {"session_id", "session-" + std::to_string(i)},
            {"workspace_hash", "workspace-a"},
        });
    }
    parsed = parse_session_reference_descriptors(many);
    EXPECT_FALSE(parsed.ok);
    EXPECT_EQ(parsed.error, "too many session references");
}

TEST(SessionReferenceContext, AcceptsNoWorkspaceWithoutClientPathScope) {
    const auto parsed = parse_session_reference_descriptors(json::array({json{
        {"session_id", "session-task"},
        {"workspace_hash", "ignored"},
        {"no_workspace", true},
        {"title", "Free task"},
        {"workspace_name", "Task"},
    }}));
    ASSERT_TRUE(parsed.ok) << parsed.error;
    ASSERT_EQ(parsed.references.size(), 1u);
    EXPECT_TRUE(parsed.references[0].no_workspace);
    EXPECT_TRUE(parsed.references[0].workspace_hash.empty());
}

TEST(SessionReferenceContext, FormatsVisibleConversationAndSkipsInternalRows) {
    ChatMessage visible_user = message("user", "expanded hidden text");
    visible_user.metadata["display_text"] = "visible user request";
    ChatMessage tool = message("tool", "large tool output");
    ChatMessage meta = message("assistant", "hidden metadata");
    meta.is_meta = true;

    ResolvedSessionReference reference;
    reference.descriptor = {
        "session-1", "workspace-a", false, "Source task", "ACECode",
    };
    reference.messages = {
        visible_user,
        tool,
        meta,
        message("assistant", "visible assistant answer"),
    };

    const auto context = build_session_reference_prompt_context({reference});
    ASSERT_EQ(context.meta.size(), 1u);
    EXPECT_NE(context.prompt.find("Title: Source task"), std::string::npos);
    EXPECT_NE(context.prompt.find("Workspace: ACECode"), std::string::npos);
    EXPECT_NE(context.prompt.find("User:\nvisible user request"), std::string::npos);
    EXPECT_NE(context.prompt.find("Assistant:\nvisible assistant answer"), std::string::npos);
    EXPECT_EQ(context.prompt.find("large tool output"), std::string::npos);
    EXPECT_EQ(context.prompt.find("hidden metadata"), std::string::npos);

    const auto augmented = build_session_reference_augmented_prompt(
        context, "compare the result");
    EXPECT_NE(augmented.find(context.prompt), std::string::npos);
    EXPECT_NE(augmented.find("Current user request:\ncompare the result"),
              std::string::npos);
}

TEST(SessionReferenceContext, BoundsLargeTranscriptsAndKeepsUtf8Valid) {
    ResolvedSessionReference reference;
    reference.descriptor = {
        "session-1", "workspace-a", false, "Large", "ACECode",
    };
    reference.messages.push_back(message(
        "user", std::string(kMaxSessionReferenceTranscriptBytes + 100, 'x') + "会"));

    const auto context = build_session_reference_prompt_context({reference});
    EXPECT_LE(context.prompt.size(), kMaxSessionReferencePromptBytes);
    EXPECT_NE(context.prompt.find("[Message truncated]"), std::string::npos);
    EXPECT_NO_THROW(static_cast<void>(json(context.prompt).dump()));
}

TEST(SessionReferenceContext, SharesTotalBudgetAcrossEveryAcceptedReference) {
    std::vector<ResolvedSessionReference> references;
    for (std::size_t index = 0; index < kMaxSessionReferences; ++index) {
        ResolvedSessionReference reference;
        reference.descriptor = {
            "session-" + std::to_string(index),
            "workspace-a",
            false,
            "Source " + std::to_string(index),
            "ACECode",
        };
        reference.messages.push_back(message(
            "user", std::string(kMaxSessionReferenceTranscriptBytes, 'x')));
        references.push_back(std::move(reference));
    }

    const auto context = build_session_reference_prompt_context(references);
    ASSERT_EQ(context.meta.size(), kMaxSessionReferences);
    EXPECT_LE(context.prompt.size(), kMaxSessionReferencePromptBytes);
    for (std::size_t index = 0; index < kMaxSessionReferences; ++index) {
        EXPECT_NE(
            context.prompt.find("Title: Source " + std::to_string(index)),
            std::string::npos);
    }
}

} // namespace
} // namespace acecode::web
