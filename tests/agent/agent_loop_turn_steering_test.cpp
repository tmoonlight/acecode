#include <gtest/gtest.h>

#include "agent/agent_loop.hpp"
#include "permissions/permissions.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/turn_timing.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "tool/tool_executor.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace fs = std::filesystem;

namespace {

fs::path steering_temp_cwd(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_turn_steering_" + hint + "_" +
         std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::remove_all(acecode::SessionStorage::get_project_dir(dir.string()));
    return dir;
}

class TurnSteeringHarness {
public:
    explicit TurnSteeringHarness(const std::string& hint)
        : cwd_(steering_temp_cwd(hint)) {
        sm_->start_session(cwd_.string(), "stub", "stub-1", "sid-" + hint);

        acecode::AgentCallbacks callbacks;
        callbacks.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(mu_);
            busy_ = busy;
            saw_busy_ = saw_busy_ || busy;
            cv_.notify_all();
        };

        auto accessor = [this]() -> std::shared_ptr<acecode::LlmProvider> {
            return provider_;
        };
        loop_ = std::make_unique<acecode::AgentLoop>(
            accessor, tools_, callbacks, cwd_.string(), permissions_);
        loop_->set_session_manager(sm_.get());
        sub_ = loop_->events().subscribe([this](const acecode::SessionEvent& event) {
            std::lock_guard<std::mutex> lk(events_mu_);
            events_.push_back(event);
        });
    }

    ~TurnSteeringHarness() {
        if (loop_ && sub_ != 0) loop_->events().unsubscribe(sub_);
        loop_.reset();
        sm_.reset(); // Close SQLite handles before deleting the project on Windows.
        fs::remove_all(cwd_);
        fs::remove_all(acecode::SessionStorage::get_project_dir(cwd_.string()));
    }

    acecode::AgentLoop& loop() { return *loop_; }
    acecode_test::StubLlmProvider& provider() { return *provider_; }
    acecode::SessionManager& session_manager() { return *sm_; }

    std::string wait_for_active_turn(std::chrono::milliseconds timeout = 5s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const std::string id = loop_->active_turn_id();
            if (!id.empty()) return id;
            std::this_thread::sleep_for(2ms);
        }
        return loop_->active_turn_id();
    }

    bool wait_until_idle(std::chrono::milliseconds timeout = 10s) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [this] {
            return saw_busy_ && !busy_;
        });
    }

    bool wait_for_provider_turns(int count,
                                 std::chrono::milliseconds timeout = 5s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (provider_->turn_count() >= count) return true;
            std::this_thread::sleep_for(2ms);
        }
        return provider_->turn_count() >= count;
    }

    bool wait_for_provider_turns_and_idle(
        int count,
        std::chrono::milliseconds timeout = 10s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (provider_->turn_count() >= count && !loop_->is_busy()) {
                std::this_thread::sleep_for(10ms);
                if (provider_->turn_count() >= count && !loop_->is_busy()) {
                    return true;
                }
            }
            std::this_thread::sleep_for(2ms);
        }
        return provider_->turn_count() >= count && !loop_->is_busy();
    }

    std::vector<acecode::SessionEvent> events() const {
        std::lock_guard<std::mutex> lk(events_mu_);
        return events_;
    }

private:
    fs::path cwd_;
    std::shared_ptr<acecode_test::StubLlmProvider> provider_ =
        std::make_shared<acecode_test::StubLlmProvider>();
    acecode::ToolExecutor tools_;
    acecode::PermissionManager permissions_;
    std::unique_ptr<acecode::SessionManager> sm_ =
        std::make_unique<acecode::SessionManager>();
    std::unique_ptr<acecode::AgentLoop> loop_;
    acecode::EventDispatcher::SubscriptionId sub_ = 0;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool busy_ = false;
    bool saw_busy_ = false;

    mutable std::mutex events_mu_;
    std::vector<acecode::SessionEvent> events_;
};

std::vector<std::string> request_user_texts(
    const std::vector<acecode::ChatMessage>& messages) {
    std::vector<std::string> texts;
    for (const auto& message : messages) {
        if (message.role == "user") texts.push_back(message.content);
    }
    return texts;
}

} // namespace

TEST(AgentLoopUserMessageRetry, ReusesStructuredTailWithoutDuplicatingUser) {
    TurnSteeringHarness h("retry-structured");
    acecode::ChatMessage user;
    user.role = "user";
    user.uuid = "retry-user-1";
    user.content = "expanded skill and selection context";
    user.content_parts = nlohmann::json::array({
        {{"type", "text"}, {"text", user.content}},
        {{"type", "image_url"}, {"image_url", {{"url", "data:image/png;base64,AAAA"}}}},
    });
    user.metadata = {{"display_text", "/skill original"},
                     {"attachments", nlohmann::json::array({{{"id", "att-1"}}})}};
    h.loop().push_message(user);
    h.session_manager().on_message(user);
    const auto timing = acecode::make_turn_timing_message(
        {user.uuid, 1, 2, 1, "aborted"}, "2026-09-18T00:00:00Z");
    h.loop().push_message(timing);
    h.session_manager().on_message(timing);

    h.provider().push_text("retried response");
    h.provider().set_latency_ms(100);
    std::string error;
    ASSERT_TRUE(h.loop().retry_last_user_message(user.uuid, error)) << error;
    EXPECT_FALSE(h.loop().retry_last_user_message(user.uuid, error));
    ASSERT_TRUE(h.wait_for_provider_turns_and_idle(1));

    const auto request = h.provider().messages_for_turn(0);
    const auto sent = std::find_if(request.begin(), request.end(), [&](const auto& message) {
        return message.uuid == user.uuid;
    });
    ASSERT_NE(sent, request.end());
    EXPECT_EQ(sent->content, user.content);
    EXPECT_EQ(sent->content_parts, user.content_parts);
    EXPECT_EQ(sent->metadata, user.metadata);
    EXPECT_EQ(request_user_texts(request), std::vector<std::string>{user.content});
    const auto persisted = h.session_manager().load_active_messages();
    EXPECT_EQ(std::count_if(persisted.begin(), persisted.end(), [](const auto& message) {
        return message.role == "user";
    }), 1);
    for (const auto& event : h.events()) {
        EXPECT_FALSE(event.kind == acecode::SessionEventKind::Message &&
                     event.payload.value("role", std::string{}) == "user");
    }
    EXPECT_FALSE(h.loop().retry_last_user_message(user.uuid, error));
    EXPECT_EQ(h.provider().turn_count(), 1);
}

TEST(AgentLoopUserMessageRetry, RejectsEveryVisibleNonUserTailIncludingEmptyAssistant) {
    for (const auto& role : {"assistant", "tool", "system", "error"}) {
        SCOPED_TRACE(role);
        TurnSteeringHarness h(std::string("retry-tail-") + role);
        acecode::ChatMessage user;
        user.role = "user";
        user.uuid = "user-before-tail";
        user.content = "original";
        h.loop().push_message(user);
        h.session_manager().on_message(user);
        acecode::ChatMessage tail;
        tail.role = role;
        // In particular, an empty assistant is still a non-user tail.
        h.loop().push_message(tail);
        h.session_manager().on_message(tail);
        std::string error;
        EXPECT_FALSE(h.loop().retry_last_user_message(user.uuid, error));
        EXPECT_EQ(h.provider().turn_count(), 0);
    }
}

TEST(AgentLoopUserMessageRetry, ChecksPersistedTranscriptNotOnlyModelHistory) {
    TurnSteeringHarness h("retry-persisted-tail");
    acecode::ChatMessage user;
    user.role = "user";
    user.uuid = "user-before-notice";
    user.content = "original";
    h.loop().push_message(user);
    h.session_manager().on_message(user);
    h.loop().emit_transcript_system_message("later transcript-only notice");
    std::string error;
    EXPECT_FALSE(h.loop().retry_last_user_message(user.uuid, error));
    EXPECT_EQ(h.provider().turn_count(), 0);
}

TEST(AgentLoopUserMessageRetry, RejectsEmptyHistoryAndStaleIdentity) {
    TurnSteeringHarness h("retry-stale-id");
    std::string error;
    EXPECT_FALSE(h.loop().retry_last_user_message("", error));
    EXPECT_FALSE(h.loop().retry_last_user_message("missing", error));
    acecode::ChatMessage user;
    user.role = "user";
    user.uuid = "latest-user";
    user.content = "latest";
    h.loop().push_message(user);
    h.session_manager().on_message(user);
    EXPECT_FALSE(h.loop().retry_last_user_message("older-user", error));
    EXPECT_EQ(h.provider().turn_count(), 0);
}

TEST(AgentLoopUserMessageRetry, RejectsVisibleEventsAbsentFromStoredHistory) {
    for (const auto kind : {acecode::SessionEventKind::Message,
                            acecode::SessionEventKind::Token,
                            acecode::SessionEventKind::Reasoning,
                            acecode::SessionEventKind::ToolStart,
                            acecode::SessionEventKind::Error}) {
        TurnSteeringHarness h("retry-live-tail");
        acecode::ChatMessage user;
        user.role = "user";
        user.uuid = "retry-live-user";
        user.content = "original";
        h.loop().push_message(user);
        h.session_manager().on_message(user);
        h.loop().events().emit(kind, {{"role", "error"}, {"text", "partial"},
                                     {"content", "error notice"}, {"reason", "failed"}});
        std::string error;
        EXPECT_FALSE(h.loop().retry_last_user_message(user.uuid, error));
        EXPECT_EQ(h.provider().turn_count(), 0);
    }
}

TEST(AgentLoopUserMessageRetry, RejectsActiveOrQueuedControlWork) {
    TurnSteeringHarness h("retry-control-work");
    acecode::ChatMessage user;
    user.role = "user";
    user.uuid = "retry-control-user";
    user.content = "original";
    h.loop().push_message(user);
    h.session_manager().on_message(user);
    std::promise<void> release;
    auto gate = release.get_future().share();
    h.loop().enqueue_control([gate] {
        gate.wait_for(2s);
        return true;
    });
    std::string error;
    EXPECT_FALSE(h.loop().retry_last_user_message(user.uuid, error));
    release.set_value();
    EXPECT_EQ(h.provider().turn_count(), 0);
}

TEST(AgentLoopUserMessageRetry, RetriesConfirmedUserAbortWithoutDuplicatingUser) {
    TurnSteeringHarness h("retry-user-abort");
    h.provider().set_latency_ms(200);
    h.provider().push_text("cancelled response");
    h.provider().push_text("retried response");
    h.loop().submit("original request");
    const auto id = h.wait_for_active_turn();
    ASSERT_TRUE(h.wait_for_provider_turns(1));
    h.loop().abort();
    ASSERT_TRUE(h.wait_for_provider_turns_and_idle(1));

    const auto stopped = h.session_manager().load_active_messages();
    ASSERT_FALSE(stopped.empty());
    EXPECT_EQ(stopped.back().metadata.value("retry_user_message_id", ""), id);
    EXPECT_TRUE(stopped.back().metadata.value("user_aborted", false));
    EXPECT_TRUE(stopped.back().metadata.value("transcript_only", false));
    std::string error;
    EXPECT_FALSE(h.loop().retry_last_user_message("stale-id", error));
    ASSERT_TRUE(h.loop().retry_last_user_message(id, error)) << error;
    EXPECT_FALSE(h.loop().retry_last_user_message(id, error));
    ASSERT_TRUE(h.wait_for_provider_turns_and_idle(2));
    EXPECT_EQ(request_user_texts(h.provider().messages_for_turn(1)),
              std::vector<std::string>{"original request"});
    const auto persisted = h.session_manager().load_active_messages();
    EXPECT_EQ(std::count_if(persisted.begin(), persisted.end(), [](const auto& message) {
        return message.role == "user";
    }), 1);
    EXPECT_FALSE(h.loop().retry_last_user_message(id, error));
}

TEST(AgentLoopUserMessageRetry, RetriesAbortAfterPartialTextOrReasoning) {
    for (const auto kind : {acecode::SessionEventKind::Token,
                            acecode::SessionEventKind::Reasoning}) {
        TurnSteeringHarness h("retry-partial-abort");
        acecode::StreamEvent partial;
        partial.type = kind == acecode::SessionEventKind::Token
            ? acecode::StreamEventType::Delta : acecode::StreamEventType::ReasoningDelta;
        partial.content = "partial output";
        h.provider().push_events({partial});
        h.provider().push_text("retried response");
        const auto sub = h.loop().events().subscribe([&](const auto& event) {
            if (event.kind == kind) h.loop().abort();
        });
        h.loop().submit("original request");
        ASSERT_TRUE(h.wait_for_provider_turns_and_idle(1));
        h.loop().events().unsubscribe(sub);
        const auto stopped = h.session_manager().load_active_messages();
        ASSERT_FALSE(stopped.empty());
        if (kind == acecode::SessionEventKind::Token) {
            const auto partial_message = std::find_if(stopped.begin(), stopped.end(), [](const auto& message) {
                return message.role == "assistant" && message.content == "partial output";
            });
            ASSERT_NE(partial_message, stopped.end());
            EXPECT_TRUE(partial_message->metadata.value("transcript_only", false));
            EXPECT_TRUE(partial_message->metadata.value("interrupted_output", false));
        }
        const auto id = stopped.back().metadata.value("retry_user_message_id", "");
        ASSERT_FALSE(id.empty());
        std::string error;
        ASSERT_TRUE(h.loop().retry_last_user_message(id, error)) << error;
        ASSERT_TRUE(h.wait_for_provider_turns_and_idle(2));
        EXPECT_EQ(request_user_texts(h.provider().messages_for_turn(1)),
                  std::vector<std::string>{"original request"});
    }
}

TEST(AgentLoopUserMessageRetry, PreservesToolResultsAndAppendsOriginalStructuredInputAfterAbort) {
    TurnSteeringHarness h("retry-tools-abort");
    // Even an unavailable tool produces an assistant call and paired result.
    h.provider().push_tool_call("unavailable_tool", "{}", "retry-tool-call");
    h.provider().push_text("retried response");
    const auto sub = h.loop().events().subscribe([&](const auto& event) {
        if (event.kind == acecode::SessionEventKind::ModelStepFinish) h.loop().abort();
    });
    acecode::UserInput input;
    input.text = "original with context";
    input.display_text = "original visible text";
    input.content_parts = nlohmann::json::array({
        {{"type", "text"}, {"text", input.text}},
        {{"type", "image_url"}, {"image_url", {{"url", "data:image/png;base64,AAAA"}}}},
    });
    input.metadata = {{"client_message_id", "original-client"}};
    h.loop().submit(input);
    ASSERT_TRUE(h.wait_for_provider_turns_and_idle(1));
    h.loop().events().unsubscribe(sub);
    const auto stopped = h.session_manager().load_active_messages();
    const auto id = stopped.back().metadata.value("retry_user_message_id", "");
    ASSERT_FALSE(id.empty());
    std::string error;
    ASSERT_TRUE(h.loop().retry_last_user_message(id, error)) << error;
    ASSERT_TRUE(h.wait_for_provider_turns_and_idle(2));
    const auto request = h.provider().messages_for_turn(1);
    ASSERT_GE(request.size(), 4u);
    const auto& retried = request.back();
    EXPECT_EQ(retried.role, "user");
    EXPECT_NE(retried.uuid, id);
    EXPECT_EQ(retried.content, input.text);
    EXPECT_EQ(retried.content_parts, input.content_parts);
    EXPECT_EQ(retried.metadata.value("display_text", ""), input.display_text);
    EXPECT_FALSE(retried.metadata.contains("client_message_id"));
    EXPECT_EQ(request[request.size() - 2].role, "tool");
    EXPECT_EQ(request[request.size() - 2].tool_call_id, "retry-tool-call");
    EXPECT_EQ(request_user_texts(request), (std::vector<std::string>{input.text, input.text}));
    for (std::size_t i = 1; i < request.size(); ++i) {
        EXPECT_FALSE(request[i - 1].role == "user" && request[i].role == "user");
        EXPECT_NE(request[i].content, "[Interrupted]");
    }
    const auto persisted = h.session_manager().load_active_messages();
    EXPECT_EQ(std::count_if(persisted.begin(), persisted.end(), [](const auto& message) {
        return message.role == "user";
    }), 2);
}

TEST(AgentLoopUserMessageRetry, RestoresExplicitAbortButRejectsPlainNoticesAndSupersededStops) {
    for (const auto& mode : {"restored", "plain", "interjected", "stale", "newer-message", "live-error", "invalid-model-tail"}) {
        SCOPED_TRACE(mode);
        TurnSteeringHarness h(std::string("retry-abort-") + mode);
        acecode::ChatMessage user;
        user.role = "user";
        user.uuid = "restored-user";
        user.content = "original";
        acecode::ChatMessage assistant;
        assistant.role = std::string(mode) == "invalid-model-tail" ? "system" : "assistant";
        assistant.content = "existing partial reply";
        for (const auto& message : {user, assistant}) {
            h.loop().push_message(message);
            h.session_manager().on_message(message);
        }
        acecode::ChatMessage marker;
        marker.role = "system";
        marker.content = "[Interrupted]";
        marker.metadata = {{"transcript_only", true}};
        const std::string variant = mode;
        if (variant != "plain" && variant != "interjected") {
            marker.metadata["user_aborted"] = true;
            marker.metadata["retry_user_message_id"] = variant == "stale" ? "older-user" : user.uuid;
        }
        if (variant == "interjected") marker.metadata["turn_interrupt"] = true;
        h.session_manager().on_message(marker);
        if (variant == "newer-message") h.loop().emit_transcript_system_message("later notice");
        if (variant == "live-error") {
            h.loop().events().emit(acecode::SessionEventKind::Error, {{"reason", "later error"}});
        }
        h.provider().push_text("retried response");
        std::string error;
        const bool accepted = h.loop().retry_last_user_message(user.uuid, error);
        EXPECT_EQ(accepted, variant == "restored") << error;
        if (accepted) {
            ASSERT_TRUE(h.wait_for_provider_turns_and_idle(1));
            const auto request = h.provider().messages_for_turn(0);
            ASSERT_GE(request.size(), 3u);
            EXPECT_EQ(request[request.size() - 2].content, assistant.content);
            EXPECT_EQ(request.back().content, user.content);
            EXPECT_NE(request.back().uuid, user.uuid);
        }
    }
}

TEST(AgentLoopTurnSteering, RejectsInvalidIdleAndMismatchedRequests) {
    TurnSteeringHarness h("reject");

    acecode::UserInput guidance;
    guidance.text = "adjust";
    EXPECT_EQ(
        h.loop().steer_input("idle-turn", guidance).status,
        acecode::TurnSteerStatus::NoActiveTurn);

    acecode::UserInput blank;
    blank.text = " \t\r\n";
    EXPECT_EQ(
        h.loop().steer_input("idle-turn", blank).status,
        acecode::TurnSteerStatus::InvalidInput);

    h.provider().set_latency_ms(120);
    h.provider().push_text("first answer");
    h.loop().submit("start");
    const std::string turn_id = h.wait_for_active_turn();
    ASSERT_FALSE(turn_id.empty());

    auto mismatch = h.loop().steer_input("another-turn", guidance);
    EXPECT_EQ(mismatch.status, acecode::TurnSteerStatus::TurnMismatch);
    EXPECT_EQ(mismatch.turn_id, turn_id);

    ASSERT_TRUE(h.wait_until_idle());
    EXPECT_TRUE(h.loop().active_turn_id().empty());
}

TEST(AgentLoopTurnSteering, CommitsStructuredInputsInFifoAndContinuesSameTurn) {
    TurnSteeringHarness h("fifo");
    h.provider().set_latency_ms(100);
    h.provider().push_text("intermediate");
    h.provider().push_text("final");

    h.loop().submit("start");
    const std::string turn_id = h.wait_for_active_turn();
    ASSERT_FALSE(turn_id.empty());

    acecode::UserInput first;
    first.text = "first guidance";
    first.metadata["client_message_id"] = "guide-1";

    acecode::UserInput second;
    second.text = "second guidance";
    second.display_text = "second visible guidance";
    second.content_parts = nlohmann::json::array({
        nlohmann::json{{"type", "text"}, {"text", "second guidance"}},
        nlohmann::json{{"type", "browser_context"},
                       {"context", nlohmann::json{{"url", "https://example.test"}}}},
    });
    second.metadata["client_message_id"] = "guide-2";

    EXPECT_TRUE(h.loop().steer_input(turn_id, first).accepted());
    EXPECT_TRUE(h.loop().steer_input(turn_id, second).accepted());
    ASSERT_TRUE(h.wait_until_idle());

    ASSERT_EQ(h.provider().turn_count(), 2);
    const auto user_texts = request_user_texts(h.provider().messages_for_turn(1));
    ASSERT_GE(user_texts.size(), 3u);
    const auto start = std::find(user_texts.begin(), user_texts.end(), "start");
    const auto first_guidance =
        std::find(user_texts.begin(), user_texts.end(), "first guidance");
    const auto second_guidance =
        std::find(user_texts.begin(), user_texts.end(), "second guidance");
    ASSERT_NE(start, user_texts.end());
    ASSERT_NE(first_guidance, user_texts.end());
    ASSERT_NE(second_guidance, user_texts.end());
    EXPECT_LT(start, first_guidance);
    EXPECT_LT(first_guidance, second_guidance);
    EXPECT_EQ(second_guidance, user_texts.end() - 1)
        << "mutable API-only context must be inserted before the final real user input";

    std::vector<acecode::ChatMessage> guided;
    for (const auto& message : h.loop().messages()) {
        if (message.metadata.is_object() &&
            message.metadata.value("turn_steer", false)) {
            guided.push_back(message);
        }
    }
    ASSERT_EQ(guided.size(), 2u);
    EXPECT_EQ(guided[0].metadata.value("client_message_id", ""), "guide-1");
    EXPECT_EQ(guided[1].metadata.value("client_message_id", ""), "guide-2");
    EXPECT_EQ(guided[1].metadata.value("display_text", ""),
              "second visible guidance");
    EXPECT_EQ(guided[1].metadata.value("turn_id", ""), turn_id);
    EXPECT_TRUE(guided[1].content_parts.is_array());
    EXPECT_EQ(guided[1].content_parts.size(), 2u);

    const auto persisted = h.session_manager().load_active_messages();
    int persisted_guidance = 0;
    for (const auto& message : persisted) {
        if (message.metadata.is_object() &&
            message.metadata.value("turn_steer", false)) {
            ++persisted_guidance;
        }
    }
    EXPECT_EQ(persisted_guidance, 2);

    bool saw_started_turn_id = false;
    bool saw_committed_client_id = false;
    for (const auto& event : h.events()) {
        if (event.kind == acecode::SessionEventKind::BusyChanged &&
            event.payload.value("busy", false) &&
            event.payload.value("turn_id", "") == turn_id) {
            saw_started_turn_id = true;
        }
        if (event.kind == acecode::SessionEventKind::Message &&
            event.payload.value("role", "") == "user" &&
            event.payload.contains("metadata") &&
            event.payload["metadata"].value("client_message_id", "") == "guide-2") {
            saw_committed_client_id = true;
        }
    }
    EXPECT_TRUE(saw_started_turn_id);
    EXPECT_TRUE(saw_committed_client_id);
}

TEST(AgentLoopTurnSteering, EveryAcceptedFinalBoundaryRaceInputIsCommitted) {
    TurnSteeringHarness h("race");
    h.provider().set_latency_ms(20);
    for (int i = 0; i < 16; ++i) {
        h.provider().push_text("response-" + std::to_string(i));
    }

    h.loop().submit("start");
    const std::string turn_id = h.wait_for_active_turn();
    ASSERT_FALSE(turn_id.empty());

    std::vector<std::string> accepted_ids;
    for (int i = 0; i < 80; ++i) {
        acecode::UserInput guidance;
        guidance.text = "race guidance " + std::to_string(i);
        const std::string client_id = "race-" + std::to_string(i);
        guidance.metadata["client_message_id"] = client_id;
        const auto result = h.loop().steer_input(turn_id, guidance);
        if (result.accepted()) {
            accepted_ids.push_back(client_id);
        } else if (result.status == acecode::TurnSteerStatus::NoActiveTurn ||
                   result.status == acecode::TurnSteerStatus::NonSteerable) {
            break;
        } else {
            FAIL() << "unexpected steer status: "
                   << acecode::to_string(result.status);
        }
        std::this_thread::sleep_for(1ms);
    }

    ASSERT_TRUE(h.wait_until_idle());
    ASSERT_FALSE(accepted_ids.empty());

    std::vector<std::string> committed_ids;
    for (const auto& message : h.loop().messages()) {
        if (!message.metadata.is_object() ||
            !message.metadata.value("turn_steer", false)) {
            continue;
        }
        committed_ids.push_back(
            message.metadata.value("client_message_id", ""));
    }
    EXPECT_EQ(committed_ids, accepted_ids);
}

TEST(AgentLoopTurnSteering, InterruptStartsStructuredTurnBeforeOrdinaryQueue) {
    TurnSteeringHarness h("interrupt_priority");
    h.provider().set_latency_ms(400);
    h.provider().push_text("old response that must be cancelled");
    h.provider().push_text("interrupt response");
    h.provider().push_text("ordinary response");

    h.loop().submit("start");
    const std::string turn_id = h.wait_for_active_turn();
    ASSERT_FALSE(turn_id.empty());
    // An active turn can still be preparing its first provider request. This
    // test interrupts an in-flight request, so wait for that precondition;
    // otherwise the replacement can legitimately be provider call number one.
    ASSERT_TRUE(h.wait_for_provider_turns(1));
    h.loop().submit("ordinary queued input");

    acecode::UserInput guidance;
    guidance.text = "replace the current approach";
    guidance.display_text = "visible replacement";
    guidance.content_parts = nlohmann::json::array({
        nlohmann::json{{"type", "text"},
                       {"text", "replace the current approach"}},
        nlohmann::json{{"type", "browser_context"},
                       {"context", nlohmann::json{{"url", "https://example.test"}}}},
    });
    guidance.metadata["client_message_id"] = "interrupt-client-1";

    const auto started = std::chrono::steady_clock::now();
    const auto result = h.loop().interrupt_turn(turn_id, guidance);
    ASSERT_TRUE(result.accepted()) << result.message;
    EXPECT_EQ(result.turn_id, turn_id);
    EXPECT_TRUE(h.loop().active_turn_id().empty())
        << "the interrupted turn must stop accepting additional steering";
    ASSERT_TRUE(h.wait_for_provider_turns(2, 250ms))
        << "the replacement provider call should start without waiting for the "
           "old 400ms response";
    EXPECT_LT(
        std::chrono::steady_clock::now() - started,
        350ms);
    ASSERT_TRUE(h.wait_for_provider_turns_and_idle(3, 5s));

    const auto replacement_request = h.provider().messages_for_turn(1);
    ASSERT_FALSE(replacement_request.empty());
    EXPECT_EQ(replacement_request.back().role, "user");
    EXPECT_EQ(replacement_request.back().content, "replace the current approach");
    EXPECT_EQ(
        replacement_request.back().metadata.value("client_message_id", ""),
        "interrupt-client-1");
    EXPECT_TRUE(
        replacement_request.back().metadata.value("turn_interrupt", false));
    EXPECT_EQ(
        replacement_request.back().metadata.value("interrupted_turn_id", ""),
        turn_id);
    EXPECT_TRUE(replacement_request.back().content_parts.is_array());
    EXPECT_EQ(replacement_request.back().content_parts.size(), 2u);

    const auto ordinary_request = h.provider().messages_for_turn(2);
    ASSERT_FALSE(ordinary_request.empty());
    EXPECT_EQ(ordinary_request.back().role, "user");
    EXPECT_EQ(ordinary_request.back().content, "ordinary queued input");

    int marker_count = 0;
    int replacement_count = 0;
    int interjected_notice_count = 0;
    for (const auto& message : h.loop().messages()) {
        if (message.metadata.is_object() &&
            message.metadata.value("turn_interrupt_marker", false)) {
            ++marker_count;
            EXPECT_TRUE(message.metadata.value("hidden_goal_context", false));
            EXPECT_NE(message.content.find("<turn_aborted>"), std::string::npos);
        }
        if (message.metadata.is_object() &&
            message.metadata.value("client_message_id", "") ==
                "interrupt-client-1") {
            ++replacement_count;
        }
        EXPECT_NE(message.content, "old response that must be cancelled");
    }
    for (const auto& event : h.events()) {
        if (event.kind != acecode::SessionEventKind::Message) continue;
        if (event.payload.value("role", "") != "system") continue;
        if (event.payload.value("content", "") != "[Interjected]") continue;
        ++interjected_notice_count;
        EXPECT_TRUE(event.payload.contains("metadata"));
        EXPECT_TRUE(event.payload["metadata"].value("turn_interrupt", false));
    }
    EXPECT_EQ(marker_count, 1);
    EXPECT_EQ(replacement_count, 1);
    EXPECT_EQ(interjected_notice_count, 1);
}

TEST(AgentLoopTurnSteering, InterruptTransfersAcceptedSoftSteersInFifo) {
    TurnSteeringHarness h("interrupt_soft_fifo");
    h.provider().set_latency_ms(200);
    h.provider().push_text("cancelled response");
    h.provider().push_text("soft steer response");
    h.provider().push_text("immediate response");

    h.loop().submit("start");
    const std::string turn_id = h.wait_for_active_turn();
    ASSERT_FALSE(turn_id.empty());

    acecode::UserInput soft;
    soft.text = "already accepted soft steer";
    soft.metadata["client_message_id"] = "soft-before-interrupt";
    ASSERT_TRUE(h.loop().steer_input(turn_id, soft).accepted());

    acecode::UserInput immediate;
    immediate.text = "immediate steer";
    immediate.metadata["client_message_id"] = "immediate-after-soft";
    ASSERT_TRUE(h.loop().interrupt_turn(turn_id, immediate).accepted());
    ASSERT_TRUE(h.wait_for_provider_turns_and_idle(3, 5s));

    const auto soft_request = h.provider().messages_for_turn(1);
    const auto immediate_request = h.provider().messages_for_turn(2);
    ASSERT_FALSE(soft_request.empty());
    ASSERT_FALSE(immediate_request.empty());
    EXPECT_EQ(soft_request.back().content, "already accepted soft steer");
    EXPECT_EQ(immediate_request.back().content, "immediate steer");
    EXPECT_TRUE(soft_request.back().metadata.value("turn_interrupt", false));
    EXPECT_TRUE(immediate_request.back().metadata.value("turn_interrupt", false));
}

TEST(AgentLoopTurnSteering, InterruptAcceptanceCommitsExactlyOnce) {
    TurnSteeringHarness h("interrupt_once");
    h.provider().set_latency_ms(30);
    h.provider().push_text("cancelled response");
    h.provider().push_text("replacement response");

    h.loop().submit("start");
    const std::string turn_id = h.wait_for_active_turn();
    ASSERT_FALSE(turn_id.empty());

    acecode::UserInput guidance;
    guidance.text = "only once";
    guidance.metadata["client_message_id"] = "interrupt-once";
    ASSERT_TRUE(h.loop().interrupt_turn(turn_id, guidance).accepted());

    const auto duplicate = h.loop().interrupt_turn(turn_id, guidance);
    EXPECT_EQ(duplicate.status, acecode::TurnSteerStatus::NonSteerable);
    ASSERT_TRUE(h.wait_for_provider_turns_and_idle(2, 3s));

    int committed = 0;
    for (const auto& message : h.loop().messages()) {
        if (message.metadata.is_object() &&
            message.metadata.value("client_message_id", "") ==
                "interrupt-once") {
            ++committed;
        }
    }
    EXPECT_EQ(committed, 1);
}
