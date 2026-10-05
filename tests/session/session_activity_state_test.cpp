#include <gtest/gtest.h>
#include "session/event_dispatcher.hpp"
#include "session/session_activity_state.hpp"

using namespace acecode;
using nlohmann::json;

TEST(SessionActivityState, CompletionSurvivesReplayEvictionAndNewTurnWakes) {
    EventDispatcher events(1);
    events.emit(SessionEventKind::BusyChanged, {{"busy", true}, {"turn_id", "first"}});
    events.emit(SessionEventKind::Done, {{"outcome", "completed"}});
    for (int i = 0; i < 5; ++i) events.emit(SessionEventKind::Token, {{"text", "x"}});
    EXPECT_EQ(events.activity_snapshot()["outcome"], "completed");
    EXPECT_FALSE(events.activity_snapshot()["busy"].get<bool>());
    events.emit(SessionEventKind::BusyChanged, {{"busy", true}, {"turn_id", "next"}});
    EXPECT_EQ(events.activity_snapshot()["outcome"], "");
    EXPECT_TRUE(events.activity_snapshot()["busy"].get<bool>());
}

TEST(SessionActivityState, PendingInteractionsOutrankOtherActivityUntilAllClose) {
    EventDispatcher events;
    events.emit(SessionEventKind::BusyChanged, {{"busy", true}});
    events.emit(SessionEventKind::ToolStart, {{"tool", "bash"}, {"tool_call_id", "t"}});
    events.emit(SessionEventKind::PermissionRequest, {{"request_id", "one"}});
    events.emit(SessionEventKind::PermissionRequest, {{"request_id", "two"}});
    events.emit(SessionEventKind::AgentProgress, {{"phase", "thinking"}, {"label", "Thinking"}});
    events.emit(SessionEventKind::PermissionClosed, {{"request_id", "one"}});
    EXPECT_EQ(events.activity_snapshot()["phase"], "permission_waiting");
    events.emit(SessionEventKind::QuestionRequest, {{"request_id", "q"}});
    events.emit(SessionEventKind::PermissionClosed, {{"request_id", "two"}});
    EXPECT_EQ(events.activity_snapshot()["phase"], "question_waiting");
    events.emit(SessionEventKind::QuestionClosed, {{"request_id", "q"}});
    EXPECT_EQ(events.activity_snapshot()["phase"], "tool_running");
    events.emit(SessionEventKind::ToolEnd, {{"tool", "bash"}, {"tool_call_id", "t"}, {"output", "private output"}});
    EXPECT_EQ(events.activity_snapshot()["tool"], "");
    EXPECT_EQ(events.activity_snapshot().dump().find("private output"), std::string::npos);
}

TEST(SessionActivityState, CancellationAndFailureAreNeverSuccessfulCompletion) {
    for (const auto* outcome : {"aborted", "error"}) {
        EventDispatcher events;
        events.emit(SessionEventKind::BusyChanged, {{"busy", true}});
        events.emit(SessionEventKind::BusyChanged, {{"busy", false}, {"outcome", outcome}});
        EXPECT_EQ(events.activity_snapshot()["outcome"], outcome);
    }
    EventDispatcher events;
    events.emit(SessionEventKind::BusyChanged, {{"busy", false}});
    EXPECT_EQ(events.activity_snapshot()["outcome"], "");
}

TEST(SessionActivityState, TransientProgressAndCompactionAreSnapshotState) {
    EventDispatcher events;
    EventDispatcher::EmitOptions transient;
    transient.buffered = false;
    events.emit(SessionEventKind::AgentProgress, {{"phase", "compacting"}}, transient);
    EXPECT_EQ(events.activity_snapshot()["phase"], "compacting");
    events.emit(SessionEventKind::Message, {{"metadata", {{"compact_notice_id", "c1"}, {"compact_notice_complete", false}}}});
    EXPECT_EQ(events.activity_snapshot()["compact_id"], "");
    events.emit(SessionEventKind::Message, {{"metadata", {{"compact_notice_id", "c1"}, {"compact_notice_complete", true}}}});
    EXPECT_EQ(events.activity_snapshot()["compact_id"], "c1");
}

TEST(SessionActivityState, MeshTransfersRetainIdentityWithoutPrivateBodyAndAreBounded) {
    EventDispatcher events;
    for (int i = 0; i < 20; ++i) events.emit(SessionEventKind::Message, {
        {"content", "private content"},
        {"metadata", {{"inter_agent", {{"type", "MESSAGE"}, {"sender", "/root/a"},
            {"recipient", "/root/b"}, {"sender_session_id", "child-a"}}}}}});
    const auto snapshot = events.activity_snapshot();
    ASSERT_EQ(snapshot["transfers"].size(), 16u);
    EXPECT_EQ(snapshot["transfers"].back()["sender_session_id"], "child-a");
    EXPECT_EQ(snapshot.dump().find("private content"), std::string::npos);
}
