#include <gtest/gtest.h>

#include "agent/compaction/compact.hpp"
#include "agent/event_payload/message_payload.hpp"
#include "agent/request/provider_history.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/turn/turn_outcome.hpp"
#include "session/event_dispatcher.hpp"
#include "session/scoped_subscription.hpp"
#include "session/session_manager.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "utils/uuid.hpp"

#include <atomic>
#include <filesystem>
#include <vector>

namespace {
class CompactionProvider : public acecode_test::StubLlmProvider {
public:
    std::vector<acecode::ChatMessage> request;
    acecode::ChatResponse chat(const std::vector<acecode::ChatMessage>& messages,
                              const std::vector<acecode::ToolDef>&) override {
        request = messages;
        acecode::ChatResponse response;
        response.content = "summary of the task";
        response.finish_reason = "stop";
        return response;
    }
};

struct ErrorSession {
    const std::filesystem::path cwd = std::filesystem::temp_directory_path() /
        ("acecode_error_transcript_" + acecode::generate_uuid());
    acecode::SessionManager session;
    std::string project_dir;

    ErrorSession() {
        std::filesystem::create_directories(cwd);
        session.start_session(cwd.string(), "stub", "stub-model");
        project_dir = session.current_project_dir();
    }
    ~ErrorSession() {
        session.end_current_session();
        std::error_code ec;
        std::filesystem::remove_all(project_dir, ec);
        std::filesystem::remove_all(cwd, ec);
    }
};
}

TEST(TranscriptWriter, ErrorsSurviveResumeWithoutEnteringProviderOrCompactionContext) {
    ErrorSession fixture;
    std::atomic<bool> busy{false};
    acecode::agent::ConversationHistory history(busy);
    acecode::EventDispatcher events;
    acecode::CallbacksSlot callbacks;
    acecode::agent::TurnOutcomeRecord outcome;
    acecode::agent::TranscriptWriter writer(history, events, callbacks, outcome, &fixture.session);
    std::vector<nlohmann::json> published;
    acecode::ScopedSubscription subscription(events, events.subscribe([&](const acecode::SessionEvent& event) {
        if (event.kind != acecode::SessionEventKind::Message) return;
        const auto disk = fixture.session.load_active_messages();
        ASSERT_FALSE(disk.empty());
        EXPECT_EQ(acecode::web::compute_message_id(disk.back()), event.payload.at("id"));
        published.push_back(event.payload);
    }));
    acecode::ChatMessage user;
    user.role = "user";
    user.content = "continue the task";
    history.append(user);
    fixture.session.on_message(user);
    const std::string error = "[Error] HTTP 451 quota exhausted";
    const nlohmann::json diagnostics = {{"provider_error", {
        {"status_code", 451}, {"raw_body", "quota exhausted"}, {"request_id", "request-451"}}}};
    // Identical failures, even in one clock interval, have distinct identities.
    for (int i = 0; i < 2; ++i) {
        writer.dispatch_message("error", error, false, diagnostics, nlohmann::json::array());
    }
    ASSERT_EQ(published.size(), 2u);
    EXPECT_NE(published[0].at("id"), published[1].at("id"));
    EXPECT_EQ(outcome.error(), error);
    const std::string sid = fixture.session.current_session_id();
    fixture.session.end_current_session();
    acecode::SessionManager resumed;
    resumed.start_session(fixture.cwd.string(), "stub", "stub-model");
    auto messages = resumed.resume_session(sid);
    ASSERT_EQ(messages.size(), 3u);
    for (std::size_t i = 1; i < messages.size(); ++i) {
        const auto& message = messages[i];
        EXPECT_EQ(message.role, "error");
        EXPECT_EQ(message.content, error);
        EXPECT_FALSE(message.timestamp.empty());
        EXPECT_FALSE(message.uuid.empty());
        EXPECT_TRUE(message.metadata.at("transcript_only").get<bool>());
        EXPECT_EQ(message.metadata.at("provider_error"), diagnostics.at("provider_error"));
        EXPECT_EQ(acecode::web::chat_message_to_payload_json(message).at("id"), published[i - 1].at("id"));
    }
    const auto provider_history = acecode::agent::detail::model_facing_provider_messages(messages, "error-resume-test");
    ASSERT_EQ(provider_history.size(), 1u);
    EXPECT_EQ(provider_history[0].content, user.content);
    CompactionProvider provider;
    const auto compacted = acecode::compact_messages(provider, messages);
    EXPECT_TRUE(compacted.performed) << compacted.error;
    ASSERT_FALSE(provider.request.empty());
    for (const auto& message : provider.request) {
        EXPECT_NE(message.role, "error");
        EXPECT_EQ(message.content.find("quota exhausted"), std::string::npos);
    }
    resumed.end_current_session();
}
