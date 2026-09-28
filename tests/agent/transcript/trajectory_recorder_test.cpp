#include <gtest/gtest.h>

#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/trajectory_recorder.hpp"
#include "session/event_dispatcher.hpp"
#include "session/session_manager.hpp"

#include <atomic>

TEST(TrajectoryRecorder, DetachesTheSingleObserverBeforeItsDependenciesAreReleased) {
    std::atomic<bool> busy{false};
    acecode::EventDispatcher events;
    acecode::agent::ConversationHistory history(busy);
    acecode::SessionManager session;
    {
        acecode::agent::TrajectoryRecorder recorder(events, history, session);
        EXPECT_EQ(events.listener_count(), 0u);
        events.emit(acecode::SessionEventKind::Token, {{"text", "partial"}});
        EXPECT_TRUE(history.retry_blocked());
        // Observer exceptions must also release their in-flight lifetime lease.
        EXPECT_NO_THROW(events.emit(acecode::SessionEventKind::Message, "malformed payload"));
    }
    history.clear_idle(true);
    events.emit(acecode::SessionEventKind::Token, {{"text", "after teardown"}});
    EXPECT_FALSE(history.retry_blocked());
    {
        acecode::agent::TrajectoryRecorder replacement(events, history, session);
        events.emit(acecode::SessionEventKind::Token, {{"text", "replacement"}});
        EXPECT_TRUE(history.retry_blocked());
    }
}
