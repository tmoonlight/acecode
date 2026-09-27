#include <gtest/gtest.h>

#include "llm/llm_provider.hpp"
#include "session/session_rewind.hpp"
#include "session/turn_net_diff.hpp"
#include "session/turn_timing.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

using acecode::ChatMessage;
using acecode::collect_rewind_targets;
using acecode::ensure_user_message_identity;
using acecode::fork_restored_prompt_text;
using acecode::is_rewind_selectable_user_message;
using acecode::retained_prefix_before_index;
using acecode::resolve_fork_anchor_index;
using acecode::rewind_prefill_text;

namespace {

ChatMessage user(std::string content) {
    ChatMessage msg;
    msg.role = "user";
    msg.content = std::move(content);
    return msg;
}

} // namespace

TEST(SessionRewind, AssignsIdentityToUserMessagesOnly) {
    ChatMessage msg = user("hello");
    ensure_user_message_identity(msg);

    EXPECT_FALSE(msg.uuid.empty());
    EXPECT_FALSE(msg.timestamp.empty());

    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.content = "hi";
    ensure_user_message_identity(assistant);
    EXPECT_TRUE(assistant.uuid.empty());
    EXPECT_TRUE(assistant.timestamp.empty());
}

TEST(SessionRewind, SelectableUserMessagesExcludeSyntheticTurns) {
    ChatMessage normal = user("fix the parser");
    EXPECT_TRUE(is_rewind_selectable_user_message(normal));

    ChatMessage meta = user("meta");
    meta.is_meta = true;
    EXPECT_FALSE(is_rewind_selectable_user_message(meta));

    ChatMessage compact = user("summary");
    compact.is_compact_summary = true;
    EXPECT_FALSE(is_rewind_selectable_user_message(compact));

    EXPECT_FALSE(is_rewind_selectable_user_message(user("!git status")));
    EXPECT_FALSE(is_rewind_selectable_user_message(user("<bash-input>ls</bash-input>")));
    EXPECT_FALSE(is_rewind_selectable_user_message(user("<task-notification>done</task-notification>")));
    EXPECT_FALSE(is_rewind_selectable_user_message(user("")));
}

TEST(SessionRewind, LegacyMessagesRemainSelectableWithoutCodeAnchor) {
    std::vector<ChatMessage> messages;
    messages.push_back(user("legacy prompt"));
    messages.push_back(user("stable prompt"));
    messages.back().uuid = "u2";

    auto targets = collect_rewind_targets(messages);

    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[0].message_index, 0u);
    EXPECT_FALSE(targets[0].has_stable_uuid);
    EXPECT_EQ(targets[1].message_index, 1u);
    EXPECT_TRUE(targets[1].has_stable_uuid);
    EXPECT_EQ(targets[1].message_uuid, "u2");
}

TEST(SessionRewind, RetainedPrefixStopsBeforeTarget) {
    std::vector<ChatMessage> messages;
    messages.push_back(user("one"));
    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.content = "answer";
    messages.push_back(assistant);
    messages.push_back(user("two"));

    auto prefix = retained_prefix_before_index(messages, 2);

    ASSERT_EQ(prefix.size(), 2u);
    EXPECT_EQ(prefix[0].content, "one");
    EXPECT_EQ(prefix[1].role, "assistant");
    EXPECT_EQ(rewind_prefill_text(messages[2]), "two");
}

TEST(SessionRewind, TargetsAndPrefixAcrossToolMetaCompactAndShellPairs) {
    std::vector<ChatMessage> messages;
    messages.push_back(user("first"));
    messages.back().uuid = "u1";

    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.content = "";
    assistant.tool_calls = nlohmann::json::array();
    messages.push_back(assistant);

    ChatMessage tool;
    tool.role = "tool";
    tool.content = "tool output";
    messages.push_back(tool);

    ChatMessage checkpoint;
    checkpoint.role = "system";
    checkpoint.is_meta = true;
    checkpoint.subtype = "file_checkpoint";
    messages.push_back(checkpoint);

    messages.push_back(user("!ls"));
    ChatMessage shell_result;
    shell_result.role = "tool_result";
    shell_result.content = "shell output";
    messages.push_back(shell_result);

    ChatMessage compact = user("compact summary");
    compact.is_compact_summary = true;
    messages.push_back(compact);

    messages.push_back(user("second"));
    messages.back().uuid = "u2";

    auto targets = collect_rewind_targets(messages);
    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[0].message_uuid, "u1");
    EXPECT_EQ(targets[1].message_uuid, "u2");

    auto prefix = retained_prefix_before_index(messages, targets[1].message_index);
    ASSERT_EQ(prefix.size(), 7u);
    EXPECT_EQ(prefix[0].content, "first");
    EXPECT_EQ(prefix[3].subtype, "file_checkpoint");
    EXPECT_EQ(prefix[4].content, "!ls");
    EXPECT_TRUE(prefix[6].is_compact_summary);
}

namespace {

ChatMessage assistant(std::string content) {
    ChatMessage msg;
    msg.role = "assistant";
    msg.content = std::move(content);
    return msg;
}

ChatMessage tool_result(std::string content) {
    ChatMessage msg;
    msg.role = "tool";
    msg.content = std::move(content);
    return msg;
}

ChatMessage assistant_with_call() {
    ChatMessage msg = assistant("");
    msg.tool_calls = nlohmann::json::array();
    msg.tool_calls.push_back(nlohmann::json{{"id", "call-1"}});
    return msg;
}

ChatMessage checkpoint_record() {
    ChatMessage msg;
    msg.role = "system";
    msg.is_meta = true;
    msg.subtype = "file_checkpoint";
    return msg;
}

ChatMessage timing_record() {
    acecode::TurnTimingRecord timing;
    timing.user_message_uuid = "u1";
    timing.duration_ms = 10;
    timing.status = "completed";
    return acecode::make_turn_timing_message(timing, "2026-09-08T00:00:00Z");
}

ChatMessage net_diff_record() {
    acecode::TurnNetDiffRecord diff;
    diff.user_message_uuid = "u1";
    return acecode::make_turn_net_diff_message(diff, "2026-09-08T00:00:00Z");
}

} // namespace

TEST(SessionRewindForkAnchor, StopsAtTheAssistantReplyBeforeThePrompt) {
    std::vector<ChatMessage> messages;
    messages.push_back(user("first"));
    messages.push_back(assistant("summary"));
    messages.push_back(checkpoint_record());
    messages.push_back(user("second"));

    // 分叉点 index 3 是 user 提示词:跳过 file_checkpoint,停在 assistant 总结。
    const auto anchor = resolve_fork_anchor_index(messages, 3);
    ASSERT_TRUE(anchor.has_value());
    EXPECT_EQ(*anchor, 1u);
}

TEST(SessionRewindForkAnchor, SkipsTurnTimingAndNetDiffRecords) {
    std::vector<ChatMessage> messages;
    messages.push_back(user("first"));
    messages.push_back(assistant("summary"));
    messages.push_back(timing_record());
    messages.push_back(net_diff_record());
    messages.push_back(user("second"));

    ASSERT_TRUE(acecode::is_turn_timing_message(messages[2]));
    ASSERT_TRUE(acecode::is_turn_net_diff_message(messages[3]));

    const auto anchor = resolve_fork_anchor_index(messages, 4);
    ASSERT_TRUE(anchor.has_value());
    EXPECT_EQ(*anchor, 1u);
}

TEST(SessionRewindForkAnchor, HasNoAnchorWhenThePromptIsTheFirstMessage) {
    std::vector<ChatMessage> messages;
    messages.push_back(user("only prompt"));
    messages.push_back(assistant("reply"));

    // 第一条消息之前无内容可留,分叉结果为空会话。
    EXPECT_FALSE(resolve_fork_anchor_index(messages, 0).has_value());
}

TEST(SessionRewindForkAnchor, KeepsTheEarlierOfConsecutivePrompts) {
    std::vector<ChatMessage> messages;
    messages.push_back(user("first"));
    messages.push_back(user("second"));
    messages.push_back(user("third"));

    // 连续输入时只丢掉被点击的那条,前面的提问保留。
    const auto anchor = resolve_fork_anchor_index(messages, 2);
    ASSERT_TRUE(anchor.has_value());
    EXPECT_EQ(*anchor, 1u);
}

TEST(SessionRewindForkAnchor, StopsAtToolResultSoTheCallStaysAnswered) {
    std::vector<ChatMessage> messages;
    messages.push_back(assistant_with_call());
    messages.push_back(tool_result("output"));
    messages.push_back(user("stop"));

    // tool 结果排在调用之后,停在结果处保留了完整配对;
    // 若跳过结果去停在 assistant 上会留下悬空 tool_call。
    const auto anchor = resolve_fork_anchor_index(messages, 2);
    ASSERT_TRUE(anchor.has_value());
    EXPECT_EQ(*anchor, 1u);
}

TEST(SessionRewindForkAnchor, SkipsAssistantWithUnansweredToolCalls) {
    std::vector<ChatMessage> messages;
    messages.push_back(assistant("safe reply"));
    messages.push_back(assistant_with_call());
    messages.push_back(user("stop"));

    // 该 assistant 的 tool 结果不存在,继续往前退,避免分叉出悬空调用。
    const auto anchor = resolve_fork_anchor_index(messages, 2);
    ASSERT_TRUE(anchor.has_value());
    EXPECT_EQ(*anchor, 0u);
}

TEST(SessionRewindForkAnchor, ReturnsNoAnchorForOutOfRangeTarget) {
    std::vector<ChatMessage> messages;
    messages.push_back(user("first"));

    EXPECT_FALSE(resolve_fork_anchor_index(messages, 1).has_value());
}

TEST(SessionRewindForkRestoredPrompt, PrefersContentWhenNonEmpty) {
    ChatMessage msg = user("plain prompt");
    msg.content_parts = nlohmann::json::array({
        nlohmann::json{{"type", "text"}, {"text", "structured"}},
    });

    // content 非空时直接使用,不看 content_parts。
    EXPECT_EQ(fork_restored_prompt_text(msg), "plain prompt");
}

TEST(SessionRewindForkRestoredPrompt, JoinsTextPartsWhenContentIsEmpty) {
    ChatMessage msg = user("");
    msg.content_parts = nlohmann::json::array({
        nlohmann::json{{"type", "text"}, {"text", "line one"}},
        nlohmann::json{{"type", "image"}, {"source", "x.png"}},
        nlohmann::json{{"type", "text"}, {"text", "line two"}},
    });

    // 文本只存于 content_parts 时拼接纯文本,附件跳过。
    EXPECT_EQ(fork_restored_prompt_text(msg), "line one\nline two");
}

TEST(SessionRewindForkRestoredPrompt, EmptyForAttachmentOnlyParts) {
    ChatMessage msg = user("");
    msg.content_parts = nlohmann::json::array({
        nlohmann::json{{"type", "image"}, {"source", "x.png"}},
    });

    // 只有附件没有文本时返回空串,前端因此跳过回填。
    EXPECT_EQ(fork_restored_prompt_text(msg), "");
}
