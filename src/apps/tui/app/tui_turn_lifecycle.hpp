#pragma once
#include "tui/app/tui_agent_attachment.hpp"
#include "tui/input/ports.hpp"
#include "tui/model/turn_observation.hpp"
#include "utils/lifetime_token.hpp"
namespace acecode { struct TuiState; class SessionManager; struct AppConfig; }
namespace acecode::tui {
class ChatViewport;
class TuiTurnLifecycle : public TuiAgentAttachment {
public:
    TuiTurnLifecycle(TuiState& state, IScreenPort& screen, ChatViewport& viewport,
        ITurnSubmitter& submitter, SessionManager& session, AppConfig& config,
        TurnObservation& observation,
        const std::function<void(const std::string&, std::string)>& title_attempt,
        const bool& notifications_ready, void* const& notification_window);
    std::function<void(bool)> busy_callback();
    std::function<void(const std::string&)> title_finished_callback();
private:
    void busy_changed(bool busy);
    void title_finished(const std::string& status);
    TuiState& state;
    IScreenPort& screen;
    ChatViewport& viewport;
    ITurnSubmitter& submitter;
    SessionManager& session_manager;
    AppConfig& config;
    TurnObservation& observation;
    const std::function<void(const std::string&, std::string)>& start_tui_auto_title_attempt;
    // Query current values at callback time; B-12 moves these into the binding.
    const bool& tui_notifications_ready;
    void* const& tui_notification_window;
    LifetimeToken lifetime_;
};
}
