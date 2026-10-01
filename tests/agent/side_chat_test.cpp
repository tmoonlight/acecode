#include <gtest/gtest.h>

#include "agent/side_question/side_chat.hpp"
#include "web/handlers/side_chat_handler.hpp"

#include <future>

namespace {

class SideStreamProvider : public acecode::LlmProvider {
public:
    std::vector<acecode::ChatMessage> received;
    std::vector<std::vector<acecode::ChatMessage>> requests;
    std::vector<std::vector<acecode::ToolDef>> tool_requests;
    std::function<void(const acecode::StreamCallback&, std::atomic<bool>*)> emit;
    int calls = 0;
    bool tool_free = true;
    bool expect_no_tools = true;

    acecode::ChatResponse chat(const std::vector<acecode::ChatMessage>&,
                               const std::vector<acecode::ToolDef>&) override { return {}; }
    void chat_stream(const std::vector<acecode::ChatMessage>& messages,
                     const std::vector<acecode::ToolDef>& tools,
                     const acecode::StreamCallback& callback,
                     std::atomic<bool>* abort) override {
        ++calls;
        received = messages;
        requests.push_back(messages);
        tool_requests.push_back(tools);
        if (expect_no_tools) EXPECT_TRUE(tools.empty());
        ASSERT_NE(abort, nullptr);
        if (emit) emit(callback, abort);
    }
    std::string name() const override { return "side-stream-test"; }
    std::string model() const override { return "side-stream-test"; }
    bool is_authenticated() override { return true; }
    bool supports_tool_free_chat() const override { return tool_free; }
    void set_model(const std::string&) override {}
};

void event(const acecode::StreamCallback& callback, acecode::StreamEventType type,
           std::string content = {}) {
    acecode::StreamEvent value;
    value.type = type;
    value.content = std::move(content);
    if (type == acecode::StreamEventType::Done) value.finish_reason = "stop";
    if (type == acecode::StreamEventType::Error) value.error = "upstream unavailable";
    callback(value);
}

void tool_call(const acecode::StreamCallback& callback, const std::string& id,
               const std::string& name, const std::string& arguments) {
    acecode::StreamEvent value;
    value.type = acecode::StreamEventType::ToolCall;
    value.tool_call = {id, name, arguments};
    value.tool_index = 0;
    callback(value);
}

void done_with_tool_calls(const acecode::StreamCallback& callback) {
    acecode::StreamEvent value;
    value.type = acecode::StreamEventType::Done;
    value.finish_reason = "tool_calls";
    callback(value);
}

// 只读工具集替身:记录收到的调用,回填固定内容。
acecode::SideChatToolset read_toolset(std::vector<acecode::ToolCall>& executed,
                                      const std::string& output = "FILE CONTENT") {
    acecode::SideChatToolset tools;
    tools.definitions = {{"file_read", "Read a file", nlohmann::json{{"type", "object"}}}};
    tools.execute = [&executed, output](const acecode::ToolCall& call, const std::atomic<bool>*) {
        executed.push_back(call);
        return acecode::ToolResult{output, true};
    };
    return tools;
}

std::vector<acecode::ChatMessage> main_context() {
    acecode::ChatMessage message;
    message.role = "system";
    message.content = "main context";
    return {message};
}

using acecode::StreamEventType;
using acecode::SideQuestionStatus;

TEST(SideChat, FollowupUsesDetachedHistoryAndRealDeltasWithoutMutatingSource) {
    auto provider = std::make_shared<SideStreamProvider>();
    provider->emit = [](const auto& callback, auto*) {
        event(callback, StreamEventType::Delta, "second ");
        event(callback, StreamEventType::Delta, "answer");
        event(callback, StreamEventType::Done);
    };
    auto context = main_context();
    std::vector<acecode::SideChatMessage> history{{"user", "first question"}, {"assistant", "first answer"}};
    std::vector<std::string> deltas;
    acecode::SideChatCancellation cancellation;
    const auto result = acecode::run_side_chat(provider, context, "  follow up  ", history,
        cancellation, [&](const auto& delta, bool reset) { EXPECT_FALSE(reset); deltas.push_back(delta); });
    EXPECT_EQ(result.response.status, SideQuestionStatus::Ok);
    EXPECT_EQ(result.response.answer, "second answer");
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(deltas, (std::vector<std::string>{"second ", "answer"}));
    ASSERT_EQ(provider->received.size(), 5u);
    EXPECT_EQ(provider->received[0].content, "main context");
    EXPECT_NE(provider->received[1].content.find("read-only side conversation"), std::string::npos);
    EXPECT_EQ(provider->received[2].content, "first question");
    EXPECT_EQ(provider->received[3].content, "first answer");
    EXPECT_EQ(provider->received[4].content, "follow up");
    EXPECT_EQ(context.size(), 1u);
    EXPECT_EQ(history.size(), 2u);
}

TEST(SideChat, RetryDiscardsProvisionalAnswerBeforeAcceptingNewDeltas) {
    auto provider = std::make_shared<SideStreamProvider>();
    provider->emit = [](const auto& callback, auto*) {
        event(callback, StreamEventType::Delta, "discard me");
        event(callback, StreamEventType::Retry);
        event(callback, StreamEventType::RetryResume);
        event(callback, StreamEventType::Delta, "replacement");
        event(callback, StreamEventType::Done);
    };
    acecode::SideChatCancellation cancellation;
    std::string visible;
    int resets = 0;
    const auto result = acecode::run_side_chat(provider, main_context(), "q", {}, cancellation,
        [&](const auto& delta, bool reset) { if (reset) { visible.clear(); ++resets; } else visible += delta; });
    EXPECT_EQ(result.response.status, SideQuestionStatus::Ok);
    EXPECT_EQ(result.response.answer, "replacement");
    EXPECT_EQ(visible, "replacement");
    EXPECT_EQ(resets, 1);
}

TEST(SideChat, StopPreservesPartialAnswerAndUsesIndependentAbort) {
    auto provider = std::make_shared<SideStreamProvider>();
    std::atomic<bool> main_abort{false};
    acecode::SideChatCancellation cancellation;
    provider->emit = [&](const auto& callback, auto* abort) {
        EXPECT_NE(abort, &main_abort);
        event(callback, StreamEventType::Delta, "partial");
        cancellation.cancel();
        EXPECT_TRUE(abort->load());
        event(callback, StreamEventType::Delta, "late output");
        event(callback, StreamEventType::Error);
    };
    const auto result = acecode::run_side_chat(provider, main_context(), "q", {}, cancellation, {});
    EXPECT_EQ(result.response.status, SideQuestionStatus::Ok);
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(result.response.answer, "partial");
    EXPECT_FALSE(main_abort.load());
}

TEST(SideChat, CancelBeforeProviderCallDoesNotInvokeModel) {
    auto provider = std::make_shared<SideStreamProvider>();
    acecode::SideChatCancellation cancellation;
    cancellation.cancel();
    const auto result = acecode::run_side_chat(provider, main_context(), "q", {}, cancellation, {});
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(result.response.status, SideQuestionStatus::Ok);
    EXPECT_EQ(provider->calls, 0);
}

TEST(SideChat, StopWakesLongRetryWait) {
    auto provider = std::make_shared<SideStreamProvider>();
    acecode::SideChatCancellation cancellation;
    std::promise<void> waiting;
    provider->emit = [&](const auto& callback, auto* abort) {
        event(callback, StreamEventType::Delta, "partial");
        waiting.set_value();
        EXPECT_TRUE(provider->wait_for_retry(std::chrono::seconds(30), abort));
    };
    auto pending = std::async(std::launch::async, [&] {
        return acecode::run_side_chat(provider, main_context(), "q", {}, cancellation, {});
    });
    const auto started = waiting.get_future().wait_for(std::chrono::seconds(2));
    if (started != std::future_status::ready) cancellation.cancel();
    ASSERT_EQ(started, std::future_status::ready);
    cancellation.cancel();
    EXPECT_EQ(pending.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_TRUE(pending.get().cancelled);
}

TEST(SideChat, CancellationDoesNotShortenMainRetryBackoffOnSharedProvider) {
    auto provider = std::make_shared<SideStreamProvider>();
    acecode::SideChatCancellation cancellation;
    cancellation.bind_provider(provider);
    std::atomic<bool> main_abort{false};
    std::promise<void> waiting;
    auto main_wait = std::async(std::launch::async, [&] {
        waiting.set_value();
        return provider->wait_for_retry(std::chrono::seconds(30), &main_abort);
    });
    EXPECT_EQ(waiting.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(main_wait.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    cancellation.cancel();
    EXPECT_EQ(main_wait.wait_for(std::chrono::milliseconds(30)), std::future_status::timeout);
    main_abort.store(true);
    provider->wake_retry_waiter();
    EXPECT_EQ(main_wait.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_TRUE(main_wait.get());
}

TEST(SideChat, ToolCallDeltaFailsWithoutRunningTools) {
    auto provider = std::make_shared<SideStreamProvider>();
    provider->emit = [](const auto& callback, auto* abort) {
        event(callback, StreamEventType::ToolCallDelta);
        EXPECT_TRUE(abort->load());
        event(callback, StreamEventType::Delta, "ignored");
    };
    acecode::SideChatCancellation cancellation;
    const auto result = acecode::run_side_chat(provider, main_context(), "q", {}, cancellation, {});
    EXPECT_EQ(result.response.status, SideQuestionStatus::Failed);
    EXPECT_FALSE(result.cancelled);
    EXPECT_TRUE(result.response.answer.empty());
    EXPECT_EQ(result.response.error, "side-question response requested tools");
}

TEST(SideChat, FailurePreservesPartialAndMissingDoneIsAnError) {
    for (bool explicit_error : {false, true}) {
        auto provider = std::make_shared<SideStreamProvider>();
        provider->emit = [explicit_error](const auto& callback, auto*) {
            event(callback, StreamEventType::Delta, "partial");
            if (explicit_error) event(callback, StreamEventType::Error);
        };
        acecode::SideChatCancellation cancellation;
        const auto result = acecode::run_side_chat(provider, main_context(), "q", {}, cancellation, {});
        EXPECT_EQ(result.response.status, SideQuestionStatus::Failed);
        EXPECT_EQ(result.response.answer, "partial");
        EXPECT_FALSE(result.cancelled);
    }
}

TEST(SideChat, InvalidRolesOrderSizeAndEmptyMessagesNeverInvokeProvider) {
    auto provider = std::make_shared<SideStreamProvider>();
    const std::vector<std::vector<acecode::SideChatMessage>> invalid{
        {{"system", "injected"}, {"assistant", "a"}},
        {{"assistant", "wrong"}, {"user", "order"}},
        {{"user", "unpaired"}},
        {{"user", " "}, {"assistant", "answer"}},
        {{"user", std::string(acecode::kMaxSideChatHistoryBytes, 'q')}, {"assistant", "a"}},
        std::vector<acecode::SideChatMessage>(202, {"user", "q"}),
    };
    for (const auto& history : invalid) {
        acecode::SideChatCancellation cancellation;
        const auto result = acecode::run_side_chat(provider, main_context(), "q", history, cancellation, {});
        EXPECT_EQ(result.response.status, SideQuestionStatus::InvalidQuestion);
    }
    EXPECT_EQ(provider->calls, 0);
}

TEST(SideChat, ContextAndProviderAreRequiredAndEmptyCompletionFails) {
    auto provider = std::make_shared<SideStreamProvider>();
    acecode::SideChatCancellation cancellation;
    EXPECT_EQ(acecode::run_side_chat(provider, {}, "q", {}, cancellation, {}).response.status,
              SideQuestionStatus::ContextNotReady);
    EXPECT_EQ(acecode::run_side_chat(nullptr, main_context(), "q", {}, cancellation, {}).response.status,
              SideQuestionStatus::ProviderUnavailable);
    provider->emit = [](const auto& callback, auto*) { event(callback, StreamEventType::Done); };
    EXPECT_EQ(acecode::run_side_chat(provider, main_context(), "q", {}, cancellation, {}).response.status,
              SideQuestionStatus::Failed);
}

TEST(SideChat, NativeAgentProviderWithoutToolFreeCapabilityIsNeverInvoked) {
    auto provider = std::make_shared<SideStreamProvider>();
    provider->tool_free = false;
    acecode::SideChatCancellation cancellation;
    const auto result = acecode::run_side_chat(provider, main_context(), "q", {}, cancellation, {});
    EXPECT_EQ(result.code, "SIDE_CHAT_PROVIDER_UNSUPPORTED");
    EXPECT_EQ(result.response.status, SideQuestionStatus::ProviderUnavailable);
    EXPECT_EQ(provider->calls, 0);
}

// 场景:侧边对话里模型先说一句、调用只读工具 file_read,拿到结果后再作答。
// 期望:工具只执行一次;第二次请求带上 assistant 工具调用与 tool 结果;回答把两步
// 正文用空行拼起来,流式增量与最终回答一致;工具事件先 running 后 success 并带路径。
// 回归:侧边对话不带工具表时,模型照着主会话历史把 bash 调用写成尖括号正文显示出来。
TEST(SideChat, ReadOnlyToolResultFeedsBackBeforeFinalAnswer) {
    auto provider = std::make_shared<SideStreamProvider>();
    provider->expect_no_tools = false;
    provider->emit = [&](const auto& callback, auto*) {
        if (provider->calls == 1) {
            event(callback, StreamEventType::Delta, "先看一下。");
            tool_call(callback, "call_1", "file_read", R"({"file_path":"src/a.cpp"})");
            done_with_tool_calls(callback);
            return;
        }
        event(callback, StreamEventType::Delta, "答案");
        event(callback, StreamEventType::Done);
    };
    std::vector<acecode::ToolCall> executed;
    std::vector<acecode::SideChatToolEvent> tool_events;
    std::string streamed;
    acecode::SideChatCancellation cancellation;
    const auto result = acecode::run_side_chat(provider, main_context(), "q", {}, cancellation,
        [&](const auto& delta, bool reset) { EXPECT_FALSE(reset); streamed += delta; },
        read_toolset(executed), [&](const auto& tool) { tool_events.push_back(tool); });
    EXPECT_EQ(result.response.status, SideQuestionStatus::Ok);
    EXPECT_EQ(result.response.answer, "先看一下。\n\n答案");
    EXPECT_EQ(streamed, result.response.answer);
    ASSERT_EQ(executed.size(), 1u);
    EXPECT_EQ(executed[0].id, "call_1");
    ASSERT_EQ(provider->calls, 2);
    ASSERT_EQ(provider->tool_requests[0].size(), 1u);
    EXPECT_EQ(provider->tool_requests[0][0].name, "file_read");
    // 指令点名可用的只读工具,并说明命令 / 编辑类工具在侧边对话里不可用。
    EXPECT_NE(provider->requests[0][1].content.find("only these read-only tools: file_read"),
              std::string::npos);
    const auto& second = provider->requests[1];
    ASSERT_GE(second.size(), 2u);
    const auto& assistant = second[second.size() - 2];
    EXPECT_EQ(assistant.role, "assistant");
    ASSERT_TRUE(assistant.tool_calls.is_array());
    ASSERT_EQ(assistant.tool_calls.size(), 1u);
    EXPECT_EQ(assistant.tool_calls[0]["function"]["name"], "file_read");
    EXPECT_EQ(second.back().role, "tool");
    EXPECT_EQ(second.back().tool_call_id, "call_1");
    EXPECT_EQ(second.back().content, "FILE CONTENT");
    ASSERT_EQ(tool_events.size(), 2u);
    EXPECT_EQ(tool_events[0].status, "running");
    EXPECT_EQ(tool_events[0].target, "src/a.cpp");
    EXPECT_EQ(tool_events[1].status, "success");
}

// 场景:provider 不扣住文本调用标记(非 OpenAI 兼容通道),模型仍把 bash 调用写成
// 尖括号正文。期望:这一步正文经 reset 丢弃,追加纠正提示(点名只读工具、说明不能
// 跑命令)后重试;最终回答不含尖括号标记,也没有执行任何工具。
TEST(SideChat, TextToolCallMarkupIsDiscardedAndCorrected) {
    auto provider = std::make_shared<SideStreamProvider>();
    provider->expect_no_tools = false;
    provider->emit = [&](const auto& callback, auto*) {
        if (provider->calls == 1) {
            event(callback, StreamEventType::Delta,
                  "<invoke name=\"bash\">\n<parameter name=\"command\">ls</parameter>\n</invoke>");
        } else {
            event(callback, StreamEventType::Delta, "plain answer");
        }
        event(callback, StreamEventType::Done);
    };
    std::vector<acecode::ToolCall> executed;
    std::string visible;
    int resets = 0;
    acecode::SideChatCancellation cancellation;
    const auto result = acecode::run_side_chat(provider, main_context(), "q", {}, cancellation,
        [&](const auto& delta, bool reset) {
            if (reset) { visible.clear(); ++resets; } else visible += delta;
        }, read_toolset(executed));
    EXPECT_EQ(result.response.status, SideQuestionStatus::Ok);
    EXPECT_EQ(result.response.answer, "plain answer");
    EXPECT_EQ(visible, "plain answer");
    EXPECT_EQ(resets, 1);
    EXPECT_TRUE(executed.empty());
    ASSERT_EQ(provider->calls, 2);
    const auto& correction = provider->requests[1].back();
    EXPECT_EQ(correction.role, "user");
    EXPECT_NE(correction.content.find("file_read"), std::string::npos);
    EXPECT_NE(correction.content.find("Shell commands"), std::string::npos);
}

// 场景:OpenAI 兼容 provider 已把标记扣住,只在 Done 上报 Rejected,本步正文为空。
// 期望:同样走纠正重试,而不是被判成「空回复」失败。
TEST(SideChat, ProviderRejectedTextToolCallTriggersCorrectionNotEmptyFailure) {
    auto provider = std::make_shared<SideStreamProvider>();
    provider->expect_no_tools = false;
    provider->emit = [&](const auto& callback, auto*) {
        if (provider->calls == 1) {
            acecode::StreamEvent done;
            done.type = StreamEventType::Done;
            done.finish_reason = "stop";
            done.text_tool_calls.outcome = acecode::TextToolCallDiagnostic::Outcome::Rejected;
            done.text_tool_calls.reason = "unknown_tool";
            done.text_tool_calls.error = "Unknown tool: bash";
            callback(done);
            return;
        }
        event(callback, StreamEventType::Delta, "ok");
        event(callback, StreamEventType::Done);
    };
    std::vector<acecode::ToolCall> executed;
    acecode::SideChatCancellation cancellation;
    const auto result = acecode::run_side_chat(provider, main_context(), "q", {}, cancellation, {},
                                               read_toolset(executed));
    EXPECT_EQ(result.response.status, SideQuestionStatus::Ok);
    EXPECT_EQ(result.response.answer, "ok");
    EXPECT_EQ(provider->calls, 2);
}

// 场景:模型每次(1 次 + 全部纠正)都写调用标记。期望:不再重试;标记前有正文时
// 只保留那段正文作为回答,没有正文时报错 —— 两种情况都不会把尖括号标记显示出来。
TEST(SideChat, PersistentTextToolCallsKeepOnlyProseOrFail) {
    for (bool with_prose : {true, false}) {
        auto provider = std::make_shared<SideStreamProvider>();
        provider->expect_no_tools = false;
        provider->emit = [with_prose](const auto& callback, auto*) {
            event(callback, StreamEventType::Delta,
                  std::string(with_prose ? "前言\n" : "") +
                  "<function_calls>\n<invoke name=\"bash\">\n</invoke>\n</function_calls>");
            event(callback, StreamEventType::Done);
        };
        std::vector<acecode::ToolCall> executed;
        acecode::SideChatCancellation cancellation;
        const auto result = acecode::run_side_chat(provider, main_context(), "q", {}, cancellation,
                                                   {}, read_toolset(executed));
        EXPECT_EQ(provider->calls, 1 + acecode::kMaxSideChatTextToolCallCorrections);
        EXPECT_EQ(result.response.answer.find('<'), std::string::npos);
        if (with_prose) {
            EXPECT_EQ(result.response.status, SideQuestionStatus::Ok);
            EXPECT_EQ(result.response.answer, "前言");
        } else {
            EXPECT_EQ(result.response.status, SideQuestionStatus::Failed);
        }
    }
}

// 场景:模型每一步都继续调用工具。期望:执行满 kMaxSideChatToolRounds 轮后,下一轮
// 的调用只回填「已达上限」而不执行,再给一轮作答;仍调用工具则以错误结束,不会无限循环。
TEST(SideChat, ToolRoundLimitStopsRunawayLoops) {
    auto provider = std::make_shared<SideStreamProvider>();
    provider->expect_no_tools = false;
    provider->emit = [&](const auto& callback, auto*) {
        tool_call(callback, "call_" + std::to_string(provider->calls), "file_read", "{}");
        done_with_tool_calls(callback);
    };
    std::vector<acecode::ToolCall> executed;
    acecode::SideChatCancellation cancellation;
    const auto result = acecode::run_side_chat(provider, main_context(), "q", {}, cancellation, {},
                                               read_toolset(executed));
    EXPECT_EQ(executed.size(), static_cast<std::size_t>(acecode::kMaxSideChatToolRounds));
    ASSERT_EQ(provider->calls, acecode::kMaxSideChatToolRounds + 2);
    EXPECT_NE(provider->requests.back().back().content.find("limit reached"), std::string::npos);
    EXPECT_EQ(result.response.status, SideQuestionStatus::Failed);
    EXPECT_NE(result.response.error.find("tool-call limit"), std::string::npos);
}

// 场景:工具之后的第二步流式中途 provider 重试。期望:reset 只丢弃第二步的临时正文,
// 第一步正文保留;按「工具事件处记下步起点、reset 截回起点」的前端规则重放结果与回答一致。
TEST(SideChat, RetryInLaterStepKeepsEarlierStepText) {
    auto provider = std::make_shared<SideStreamProvider>();
    provider->expect_no_tools = false;
    provider->emit = [&](const auto& callback, auto*) {
        if (provider->calls == 1) {
            event(callback, StreamEventType::Delta, "A");
            tool_call(callback, "call_1", "file_read", "{}");
            done_with_tool_calls(callback);
            return;
        }
        event(callback, StreamEventType::Delta, "draft");
        event(callback, StreamEventType::Retry);
        event(callback, StreamEventType::RetryResume);
        event(callback, StreamEventType::Delta, "B");
        event(callback, StreamEventType::Done);
    };
    std::vector<acecode::ToolCall> executed;
    std::string visible;
    std::size_t step_start = 0;
    acecode::SideChatCancellation cancellation;
    const auto result = acecode::run_side_chat(provider, main_context(), "q", {}, cancellation,
        [&](const auto& delta, bool reset) {
            if (reset) visible.resize(step_start); else visible += delta;
        }, read_toolset(executed), [&](const auto& tool) {
            if (tool.status == "running") step_start = visible.size();
        });
    EXPECT_EQ(result.response.status, SideQuestionStatus::Ok);
    EXPECT_EQ(result.response.answer, "A\n\nB");
    EXPECT_EQ(visible, result.response.answer);
}

// 场景:工具执行期间用户点停止。期望:不再发起下一步模型请求,结果为 cancelled,
// 已经流出的正文保留。
TEST(SideChat, StopDuringToolExecutionEndsWithoutAnotherModelCall) {
    auto provider = std::make_shared<SideStreamProvider>();
    provider->expect_no_tools = false;
    provider->emit = [&](const auto& callback, auto*) {
        event(callback, StreamEventType::Delta, "A");
        tool_call(callback, "call_1", "file_read", "{}");
        done_with_tool_calls(callback);
    };
    acecode::SideChatCancellation cancellation;
    acecode::SideChatToolset tools;
    tools.definitions = {{"file_read", "Read a file", nlohmann::json{{"type", "object"}}}};
    tools.execute = [&](const acecode::ToolCall&, const std::atomic<bool>* abort) {
        cancellation.cancel();
        EXPECT_TRUE(abort && abort->load());
        return acecode::ToolResult{"x", true};
    };
    const auto result = acecode::run_side_chat(provider, main_context(), "q", {}, cancellation, {},
                                               tools);
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(result.response.answer, "A");
    EXPECT_EQ(provider->calls, 1);
}

TEST(SideChatProtocol, ParsesTextHistoryAndRejectsInjectedFieldsOrMalformedTypes) {
    using nlohmann::json;
    const json valid{{"request_id", "r1"}, {"session_id", "s1"}, {"question", "next"},
                     {"history", {{{"role", "user"}, {"content", "q"}},
                                  {{"role", "assistant"}, {"content", "a"}}}}};
    EXPECT_TRUE(acecode::web::parse_side_chat_start(valid).error.empty());
    for (auto invalid : {json(nullptr), json::array(), json{{"request_id", 5}}}) {
        EXPECT_FALSE(acecode::web::parse_side_chat_start(invalid).error.empty());
    }
    auto tool_history = valid;
    tool_history["history"][1]["tool_calls"] = json::array();
    EXPECT_FALSE(acecode::web::parse_side_chat_start(tool_history).error.empty());
    auto oversized = valid;
    oversized["request_id"] = std::string(129, 'x');
    EXPECT_FALSE(acecode::web::parse_side_chat_start(oversized).error.empty());
}

TEST(SideChatProtocol, ConnectionAllowsOnlyOneActiveRequestAndStopsOnlyMatchingId) {
    acecode::web::SideChatConnectionState connection;
    auto first = connection.start("first");
    ASSERT_TRUE(first);
    EXPECT_FALSE(connection.start("second"));
    connection.stop("other");
    EXPECT_FALSE(first->cancellation.aborted.load());
    connection.stop("first");
    EXPECT_TRUE(first->cancellation.aborted.load());
    EXPECT_FALSE(connection.start("second"));
    int sent = 0;
    EXPECT_TRUE(connection.deliver(first, true, [&] { ++sent; }));
    auto second = connection.start("second");
    EXPECT_TRUE(second);
    EXPECT_FALSE(connection.deliver(first, true, [&] { ++sent; }));
    EXPECT_EQ(sent, 1);
}

TEST(SideChatProtocol, ReusedRequestIdDoesNotAcceptOldCallbackOrReleaseNewRequest) {
    acecode::web::SideChatConnectionState connection;
    auto old_request = connection.start("same-id");
    ASSERT_TRUE(connection.deliver(old_request, true, {}));
    auto new_request = connection.start("same-id");
    ASSERT_TRUE(new_request);
    EXPECT_FALSE(connection.deliver(old_request, false, [] { ADD_FAILURE(); }));
    EXPECT_FALSE(connection.deliver(old_request, true, [] { ADD_FAILURE(); }));
    EXPECT_FALSE(connection.start("another"));
    EXPECT_TRUE(connection.deliver(new_request, true, {}));
}

TEST(SideChatProtocol, DisconnectCancelsWorkerAndRejectsEveryLateDelivery) {
    acecode::web::SideChatConnectionState connection;
    auto request = connection.start("request");
    auto provider = std::make_shared<SideStreamProvider>();
    std::promise<void> waiting;
    provider->emit = [&](const auto& callback, auto* abort) {
        event(callback, StreamEventType::Delta, "partial");
        waiting.set_value();
        EXPECT_TRUE(provider->wait_for_retry(std::chrono::seconds(30), abort));
    };
    auto worker = std::async(std::launch::async, [&] {
        return acecode::run_side_chat(provider, main_context(), "q", {}, request->cancellation,
            [&](const auto&, bool) { connection.deliver(request, false, {}); });
    });
    const auto started = waiting.get_future().wait_for(std::chrono::seconds(2));
    if (started != std::future_status::ready) connection.close();
    ASSERT_EQ(started, std::future_status::ready);
    connection.close();
    EXPECT_EQ(worker.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_TRUE(worker.get().cancelled);
    EXPECT_FALSE(connection.start("new-request"));
    EXPECT_FALSE(connection.deliver(request, false, [] { ADD_FAILURE(); }));
    EXPECT_FALSE(connection.deliver(request, true, [] { ADD_FAILURE(); }));
    acecode::web::SideChatConnectionState replacement;
    EXPECT_TRUE(replacement.start("request"));
    EXPECT_FALSE(replacement.deliver(request, true, [] { ADD_FAILURE(); }));
}

TEST(SideChatProtocol, ConcurrentStartsHaveOneOwnerAndCloseWaitsForCurrentDelivery) {
    acecode::web::SideChatConnectionState connection;
    auto claim_one = std::async(std::launch::async, [&] { return connection.start("one"); });
    auto claim_two = std::async(std::launch::async, [&] { return connection.start("two"); });
    auto one = claim_one.get();
    auto two = claim_two.get();
    ASSERT_NE(static_cast<bool>(one), static_cast<bool>(two));
    auto active = one ? one : two;
    std::promise<void> sending;
    std::promise<void> release;
    auto released = release.get_future();
    auto delivery = std::async(std::launch::async, [&] {
        return connection.deliver(active, false, [&] {
            sending.set_value();
            EXPECT_EQ(released.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        });
    });
    const auto started = sending.get_future().wait_for(std::chrono::seconds(2));
    if (started != std::future_status::ready) release.set_value();
    ASSERT_EQ(started, std::future_status::ready);
    auto close = std::async(std::launch::async, [&] { connection.close(); });
    EXPECT_EQ(close.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    release.set_value();
    EXPECT_TRUE(delivery.get());
    close.get();
    EXPECT_TRUE(active->cancellation.aborted.load());
    EXPECT_FALSE(connection.deliver(active, true, [] { ADD_FAILURE(); }));
}

} // namespace
