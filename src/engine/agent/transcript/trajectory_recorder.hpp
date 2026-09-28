#pragma once

#include "utils/lifetime_token.hpp"

#include <nlohmann/json_fwd.hpp>

namespace acecode {
class EventDispatcher;
class SessionManager;
struct SessionEvent;
}

namespace acecode::agent {
class ConversationHistory;

// Exclusively owns EventDispatcher's single observer slot. This is not a
// multicast subscription. Destroy before installing a replacement recorder;
// detach and revoke wait for admitted deliveries before dependencies disappear.
class TrajectoryRecorder {
public:
    TrajectoryRecorder(EventDispatcher& events, ConversationHistory& history, SessionManager& session);
    ~TrajectoryRecorder();
    TrajectoryRecorder(const TrajectoryRecorder&) = delete;
    TrajectoryRecorder& operator=(const TrajectoryRecorder&) = delete;
    void record_terminal(nlohmann::json busy_payload, nlohmann::json done_payload);

private:
    void observe(const SessionEvent& event);
    EventDispatcher& events_;
    ConversationHistory& history_;
    SessionManager& session_;
    LifetimeToken lifetime_; // Last: revoke before borrowed dependencies are released.
};

} // namespace acecode::agent
