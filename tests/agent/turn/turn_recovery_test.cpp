#include <gtest/gtest.h>
#include "agent/callbacks_slot.hpp"
#include "agent/turn/turn_context.hpp"
#include "agent/turn/turn_finalizer.hpp"
#include "agent/turn/active_turn_gate.hpp"
#include "agent/turn/turn_outcome.hpp"
#include "agent/turn/response_recovery.hpp"
#include "agent/boundary/workspace_boundary.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "agent/hook_bridge/agent_hook_bridge.hpp"
#include "agent/progress/activity_narrator.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/worker/agent_task_queue.hpp"
#include "permissions/permissions.hpp"
#include "session/event_dispatcher.hpp"
#include "tool/tool_executor.hpp"
#include "utils/abort_signal.hpp"
#include <memory>
#include <stdexcept>

namespace {
using namespace acecode;
using namespace acecode::agent;
class TurnRecoveryTest : public testing::Test {
protected:
    std::atomic<bool> busy{true};
    std::atomic<bool> interrupt{false};
    AbortSignal abort;
    AgentCallbacks callbacks;
    acecode::CallbacksSlot callback_slot;
    PermissionManager permissions;
    ToolExecutor tools;
    ToolCapabilityPolicy policy;
    EventDispatcher events;
    ConversationHistory history{busy};
    TurnOutcomeRecord outcome;
    TranscriptWriter transcript{history, events, callback_slot, outcome};
    AgentTaskQueue queue{busy};
    ActiveTurnGate gate{busy, abort, interrupt};
    WorkspaceBoundary boundary{".", permissions};
    AgentHookBridge hooks{boundary, permissions, {}, transcript, history, abort};
    GoalRuntime goal{queue, history, transcript, events, callback_slot, permissions, busy, abort};
    ActivityNarrator activity{callback_slot};
    TurnFinalizer finalizer{{history, transcript, outcome, gate, goal, hooks, activity,
        events, callback_slot, tools, policy, busy, abort, interrupt, nullptr}};
    ResponseRecovery recovery{history, transcript, goal, nullptr};
};
}

TEST_F(TurnRecoveryTest, MissingContextAndThrowingObserversStillPublishTerminalEvents) {
    // 任务在回合建立前失败、且错误展示再次抛出时,仍需解锁并发送 Done。
    auto seen = std::make_shared<std::vector<SessionEvent>>();
    const auto subscription = events.subscribe([seen](const auto& event) { seen->push_back(event); });
    gate.begin("failed-turn");
    callbacks.on_message = [](const auto&, const auto&, bool) { throw std::runtime_error("message"); };
    callback_slot.publish(callbacks);
    callbacks.on_turn_finished = [](const auto&) { throw std::runtime_error("finished"); };
    callback_slot.publish(callbacks);
    callbacks.on_busy_changed = [](bool) { throw std::runtime_error("busy"); };
    callback_slot.publish(callbacks);
    EXPECT_NO_THROW(finalizer.recover(nullptr, "task", true, {}));
    events.unsubscribe(subscription);
    ASSERT_EQ(seen->size(), 2u);
    EXPECT_EQ((*seen)[0].kind, SessionEventKind::BusyChanged);
    EXPECT_EQ((*seen)[1].kind, SessionEventKind::Done);
    EXPECT_EQ((*seen)[1].payload.at("turn_id"), "failed-turn");
    EXPECT_TRUE((*seen)[1].payload.contains("usage"));
    EXPECT_FALSE(busy.load());
    EXPECT_TRUE(gate.id().empty());
    EXPECT_TRUE(outcome.failed());
}

TEST_F(TurnRecoveryTest, NonChatRecoveryDoesNotInventTurnUsageOrFinishedCallback) {
    // shell / compact / control 的失败不能伪装成普通对话回合完成。
    auto seen = std::make_shared<std::vector<SessionEvent>>();
    auto finished = std::make_shared<int>(0);
    callbacks.on_turn_finished = [finished](const auto&) { ++*finished; };
    callback_slot.publish(callbacks);
    const auto subscription = events.subscribe([seen](const auto& event) {
        if (event.kind == SessionEventKind::Done) seen->push_back(event);
    });
    finalizer.recover(nullptr, "control", false, {});
    events.unsubscribe(subscription);
    ASSERT_EQ(seen->size(), 1u);
    EXPECT_EQ((*seen)[0].payload.at("outcome"), "error");
    EXPECT_FALSE((*seen)[0].payload.contains("usage"));
    EXPECT_FALSE((*seen)[0].payload.contains("turn_id"));
    EXPECT_EQ(*finished, 0);
}

TEST_F(TurnRecoveryTest, RejectedBlankDsmlUsesOneCorrectionBeforeError) {
    // 被 provider 隐藏的文本调用可能只剩空白,必须优先纠正调用格式。
    ChatResponse response;
    response.content = "  ";
    response.text_tool_calls.outcome = TextToolCallDiagnostic::Outcome::Rejected;
    response.text_tool_calls.format = "dsml";
    response.text_tool_calls.reason = "invalid";
    response.text_tool_calls.error = "native calls required";
    ResponseRecoveryState state;
    const auto first = recovery.resolve(response, state, {"read_file"});
    EXPECT_EQ(first.action, HandleErrorResult::Continue);
    EXPECT_EQ(first.finish_status, "text_tool_call_retry");
    EXPECT_EQ(state.empty_response_retries, 0);
    ASSERT_GE(history.view().size(), 2u);
    EXPECT_TRUE(history.view()[1].metadata.value("hidden_goal_context", false));
    const auto second = recovery.resolve(response, state, {"read_file"});
    EXPECT_EQ(second.action, HandleErrorResult::Break);
    EXPECT_EQ(second.finish_status, "error");
    EXPECT_EQ(state.empty_response_retries, 0);
}

TEST_F(TurnRecoveryTest, StructuredOutputIsNotRetriedAsBlankText) {
    // 图片等结构化内容即使没有正文,也属于有效回复。
    ChatResponse response;
    response.content_parts = nlohmann::json::array({{{"type", "image"}, {"data", "image"}}});
    ResponseRecoveryState state;
    EXPECT_EQ(recovery.resolve(response, state, {}).action, HandleErrorResult::Proceed);
    EXPECT_TRUE(history.view().empty());
    response.content_parts = nlohmann::json::array({{{"type", 1}}});
    EXPECT_EQ(recovery.resolve(response, state, {}).action, HandleErrorResult::Proceed);
}

TEST_F(TurnRecoveryTest, ResponsesReasoningOnlyIsRetriedAndOpaqueStateIsPreserved) {
    ChatResponse response;
    response.finish_reason = "length";
    response.reasoning_content = "Still considering the request.";
    response.content_parts = nlohmann::json::array({
        {{"type", "openai_responses_item"}, {"item", {
            {"type", "reasoning"}, {"id", "rs_1"},
            {"summary", nlohmann::json::array()},
            {"encrypted_content", "opaque-state"}}}},
    });
    ResponseRecoveryState state;
    const auto result = recovery.resolve(response, state, {});
    EXPECT_EQ(result.action, HandleErrorResult::Continue);
    EXPECT_EQ(result.finish_status, "empty_response_retry");
    ASSERT_EQ(history.view().size(), 2u);
    EXPECT_EQ(history.view()[0].content_parts, response.content_parts);
    EXPECT_EQ(history.view()[0].reasoning_content, response.reasoning_content);
    EXPECT_TRUE(history.view()[1].metadata.value("empty_response_retry", false));
}

TEST_F(TurnRecoveryTest, PlaceholderOnlyOutputRetriesThenFailsInsteadOfCompleting) {
    ChatResponse response;
    response.content = "  ...\xE2\x8E\xAF\xE2\x8E\xAF\xE2\x8E\xAF";
    response.finish_reason = "stop";
    ResponseRecoveryState state;
    EXPECT_EQ(recovery.resolve(response, state, {}).action, HandleErrorResult::Continue);
    EXPECT_EQ(recovery.resolve(response, state, {}).action, HandleErrorResult::Continue);
    const auto exhausted = recovery.resolve(response, state, {});
    EXPECT_EQ(exhausted.action, HandleErrorResult::Break);
    EXPECT_EQ(exhausted.finish_status, "error");
    for (const auto& message : history.view()) {
        if (message.role == "assistant") EXPECT_TRUE(message.content.empty());
    }
}

TEST_F(TurnRecoveryTest, UsefulShortAndSymbolicAnswersAreNotPlaceholders) {
    for (const std::string content : {"OK", "0", "收到", "-1", "1 + 1 = 2", "-", "[]", "...done", "```\n---\n```"}) {
        ChatResponse response;
        response.content = content;
        ResponseRecoveryState state;
        EXPECT_EQ(recovery.resolve(response, state, {}).action, HandleErrorResult::Proceed) << content;
    }
    EXPECT_TRUE(history.view().empty());
}
