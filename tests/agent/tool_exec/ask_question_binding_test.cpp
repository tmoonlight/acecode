#include <gtest/gtest.h>
#include "agent/tool_exec/ask_question_binding.hpp"
#include "agent/agent_callbacks.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/turn/turn_outcome.hpp"
#include "agent/worker/agent_task_queue.hpp"
#include "config/config.hpp"
#include "permissions/permissions.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/event_dispatcher.hpp"
#include "session/session_manager.hpp"
#include "test_support/agent_loop/characterization_fixture.hpp"
#include "utils/abort_signal.hpp"
#include "utils/scope_exit.hpp"

namespace {
class QuestionBindingTest : public testing::Test {
protected:
    acecode_test::characterization::Isolation isolation;
    std::atomic<bool> busy{true};
    acecode::AbortSignal abort;
    acecode::AgentCallbacks callbacks;
    acecode::PermissionManager permissions;
    acecode::EventDispatcher events;
    acecode::SessionManager session;
    acecode::agent::ConversationHistory history{busy};
    acecode::agent::TurnOutcomeRecord outcome;
    acecode::agent::TranscriptWriter transcript{history, events, callbacks, outcome};
    acecode::agent::AgentTaskQueue queue{busy};
    acecode::agent::GoalRuntime goal{queue, history, transcript, events, callbacks, permissions, busy, abort};
    acecode::AgentLoopConfig config;
    acecode::agent::ProgressEmitter progress = [](const auto&...) {};
    acecode::ToolCall call{"id", "AskUserQuestion", "{}"};
};
}

TEST_F(QuestionBindingTest, TuiTimeoutIsBoundOnceAndRetainedCallbackIsCancelled) {
    config.question_policy = "timeout";
    config.question_policy_explicit = true;
    config.question_timeout_seconds = 60;
    auto timeouts = std::make_shared<std::vector<int>>();
    acecode::ToolContext context;
    {
        acecode::agent::AskQuestionBinding binding(
            goal, abort, config, nullptr, nullptr,
            [timeouts](const auto&, auto*, int timeout, const auto&) {
                timeouts->push_back(timeout);
                return nlohmann::json{{"cancelled", false}};
            });
        binding.bind(context, call, 0, progress);
        config.question_timeout_seconds = 240;
        EXPECT_FALSE(context.ask_user_questions(nlohmann::json::array())["cancelled"].get<bool>());
    }
    EXPECT_TRUE(context.ask_user_questions(nlohmann::json::array())["cancelled"].get<bool>());
    EXPECT_EQ(*timeouts, std::vector<int>{60});
}

TEST_F(QuestionBindingTest, DaemonChecksGoalWhenInvokedAndPreservesNotAnswered) {
    session.start_session(acecode::path_to_utf8(isolation.directory.path), "stub", "stub");
    session.ensure_active_session_id();
    acecode::AskUserQuestionPrompter prompter(events);
    acecode::LifetimeToken prompter_lifetime;
    const auto ref = prompter_lifetime.ref(prompter);
    auto timeouts = std::make_shared<std::vector<int>>();
    const auto subscription = events.subscribe([ref, timeouts](const acecode::SessionEvent& event) {
        if (event.kind != acecode::SessionEventKind::QuestionRequest) return;
        timeouts->push_back(event.payload["timeout_ms"].get<int>());
        ref.with([&](acecode::AskUserQuestionPrompter& target) {
            acecode::AskUserQuestionResponse response;
            response.answers.push_back({"question", {}, {}});
            target.notify_response(event.payload["request_id"].get<std::string>(), response);
        });
    });
    acecode::ScopeExit unsubscribe([&events = events, subscription] {
        events.unsubscribe(subscription);
    });
    acecode::agent::AskQuestionBinding binding(goal, abort, config, &session, &prompter, {});
    acecode::ToolContext context;
    binding.bind(context, call, 0, progress);
    ASSERT_TRUE(session.goal_store()->replace_thread_goal(
        session.current_session_id(), "goal created after binding", std::nullopt,
        acecode::ThreadGoalStatus::Active));
    const auto response = context.ask_user_questions(nlohmann::json::array());
    EXPECT_EQ(*timeouts, std::vector<int>{30000});
    ASSERT_EQ(response["answers"].size(), 1U);
    EXPECT_EQ(response["answers"][0]["not_answered"], true);
}
