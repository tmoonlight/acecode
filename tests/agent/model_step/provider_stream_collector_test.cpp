#include <gtest/gtest.h>
#include "agent/callbacks_slot.hpp"
#include "agent/turn/turn_context.hpp"
#include "agent/turn/turn_model_step_sink.hpp"
#include "agent/model_step/provider_stream_collector.hpp"
#include "agent/model_step/active_provider_slot.hpp"
#include "agent/model_step/model_step_recorder.hpp"
#include "agent/model_step/turn_usage_accountant.hpp"
#include "agent/progress/activity_narrator.hpp"
#include "agent/progress/retry_progress.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/turn/turn_outcome.hpp"
#include "agent/worker/agent_task_queue.hpp"
#include "permissions/permissions.hpp"
#include "session/event_dispatcher.hpp"
#include "tool/tool_executor.hpp"
#include "utils/abort_signal.hpp"
#include "test_support/agent/stub_provider.hpp"
#include <memory>
#include <stdexcept>

namespace {
class RetainingProvider : public acecode_test::StubLlmProvider {
public:
    acecode::StreamCallback retained;
    void chat_stream(const std::vector<acecode::ChatMessage>& messages,
                     const std::vector<acecode::ToolDef>& tools,
                     const acecode::StreamCallback& callback,
                     std::atomic<bool>* abort) override {
        retained = callback;
        StubLlmProvider::chat_stream(messages, tools, callback, abort);
    }
};
class StreamCollectorTest : public testing::Test {
protected:
    std::atomic<bool> busy{true};
    std::atomic<int> context_tokens{0};
    acecode::AbortSignal abort;
    acecode::AgentCallbacks callbacks;
    acecode::CallbacksSlot callback_slot;
    acecode::PermissionManager permissions;
    acecode::ToolExecutor tools;
    acecode::EventDispatcher events;
    acecode::agent::ConversationHistory history{busy};
    acecode::agent::TurnOutcomeRecord outcome;
    acecode::agent::TranscriptWriter transcript{history, events, callback_slot, outcome};
    acecode::agent::AgentTaskQueue queue{busy};
    acecode::agent::GoalRuntime goal{queue, history, transcript, events, callback_slot, permissions, busy, abort};
    acecode::agent::ActiveProviderSlot active_provider;
    acecode::agent::ActivityNarrator activity{callback_slot};
    acecode::agent::RetryProgressReporter retry{callback_slot, events};
    acecode::agent::TurnUsageAccountant usage{goal, callback_slot, events, context_tokens};
    acecode::agent::ModelStepRecorder recorder{tools, events};
    acecode::agent::ProviderStreamCollector collector{tools, callback_slot, events, history,
        active_provider, abort, activity, retry};
    acecode::agent::TurnContext turn{callbacks};
    acecode::agent::TurnModelStepSink sink{turn, usage, recorder, nullptr};
    acecode::agent::ProgressEmitter progress = [](const auto&...) {};
};
}

TEST_F(StreamCollectorTest, RetainedCallbackCannotAccessCompletedRequest) {
    // Provider 意外保留流回调时,请求返回后调用必须被寿命门抑制。
    auto provider = std::make_shared<RetainingProvider>();
    auto deltas = std::make_shared<std::string>();
    callbacks.on_delta = [deltas](const std::string& text) { *deltas += text; };
    callback_slot.publish(callbacks);
    provider->push_text("first");
    const auto result = collector.collect(provider, {}, progress, 1, sink, nullptr);
    EXPECT_EQ(result.accumulated.content, "first");
    acecode::StreamEvent late;
    late.type = acecode::StreamEventType::Delta;
    late.content = "late";
    ASSERT_TRUE(provider->retained);
    provider->retained(late);
    EXPECT_EQ(*deltas, "first");
}

TEST_F(StreamCollectorTest, UsageIsRetainedBeforeAConsumerThrows) {
    // 上层恢复在回调异常后仍能读取已计入的用量,不会被栈展开清空。
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    acecode::StreamEvent event;
    event.type = acecode::StreamEventType::Usage;
    event.usage.has_data = true;
    event.usage.prompt_tokens = 8;
    event.usage.completion_tokens = 3;
    event.usage.total_tokens = 11;
    provider->push_events({event});
    callbacks.on_usage = [](const auto&) { throw std::runtime_error("consumer"); };
    callback_slot.publish(callbacks);
    EXPECT_THROW(collector.collect(provider, {}, progress, 1, sink, nullptr), std::runtime_error);
    EXPECT_TRUE(turn.usage.initialized);
    EXPECT_EQ(turn.usage.aggregate.total_tokens, 11);
    EXPECT_EQ(context_tokens.load(), 11);
}
