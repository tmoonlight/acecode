#include <gtest/gtest.h>

#include "session/side_chat.hpp"
#include "web/handlers/side_chat_handler.hpp"

#include <future>

namespace {

class SideStreamProvider : public acecode::LlmProvider {
public:
    std::vector<acecode::ChatMessage> received;
    std::function<void(const acecode::StreamCallback&, std::atomic<bool>*)> emit;
    int calls = 0;
    bool tool_free = true;

    acecode::ChatResponse chat(const std::vector<acecode::ChatMessage>&,
                               const std::vector<acecode::ToolDef>&) override { return {}; }
    void chat_stream(const std::vector<acecode::ChatMessage>& messages,
                     const std::vector<acecode::ToolDef>& tools,
                     const acecode::StreamCallback& callback,
                     std::atomic<bool>* abort) override {
        ++calls;
        received = messages;
        EXPECT_TRUE(tools.empty());
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
