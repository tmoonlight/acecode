#include "trajectory_recorder.hpp"

#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_queries.hpp"
#include "session/event_dispatcher.hpp"
#include "session/session_manager.hpp"
#include "utils/time.hpp"

#include <utility>

namespace acecode::agent {

TrajectoryRecorder::TrajectoryRecorder(
    EventDispatcher& events, ConversationHistory& history, SessionManager& session)
    : events_(events), history_(history), session_(session) {
    events_.set_observer([owner = lifetime_.ref(*this)](const SessionEvent& event) {
        owner.with([&event](TrajectoryRecorder& recorder) { recorder.observe(event); });
    });
}

TrajectoryRecorder::~TrajectoryRecorder() {
    events_.set_observer({});
    lifetime_.revoke();
}

void TrajectoryRecorder::observe(const SessionEvent& event) {
    history_.observe_transcript(event);
    if (!detail::should_persist_trajectory_event(event)) return;
    session_.record_trajectory_event(to_string(event.kind), event.payload, event.timestamp_ms);
}

void TrajectoryRecorder::record_terminal(nlohmann::json busy_payload, nlohmann::json done_payload) {
    const std::int64_t timestamp_ms = utils::now_epoch_ms();
    session_.record_trajectory_event("busy_changed", std::move(busy_payload), timestamp_ms);
    session_.record_trajectory_event("done", std::move(done_payload), timestamp_ms);
}

} // namespace acecode::agent
