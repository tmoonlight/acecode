#pragma once
#include "tui/app/tui_agent_attachment.hpp"
#include "tui/input/ports.hpp"
#include "tui/model/turn_observation.hpp"
#include "utils/lifetime_token.hpp"
namespace acecode { struct TuiState; class SessionManager; struct AppConfig; class AutoTitleRunner; }
namespace acecode::tui {
class ChatViewport; class TuiNotificationBinding;
class TuiTurnLifecycle : public TuiAgentAttachment {
public:
    TuiTurnLifecycle(TuiState& state, IScreenPort& screen, ChatViewport& viewport,
        ITurnSubmitter& submitter, SessionManager& session, AppConfig& config,
        TurnObservation& observation,
        const std::unique_ptr<AutoTitleRunner>& title_runner,
        const std::unique_ptr<TuiNotificationBinding>& notifications);
    std::function<void(bool)> busy_callback();
    std::function<void(const std::string&)> title_finished_callback();
    std::function<void(const std::string&, const std::string&)> title_applied_callback();
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
    // Fixed owner slots; startup emplaces at the original activation points.
    const std::unique_ptr<AutoTitleRunner>& title_runner_;
    const std::unique_ptr<TuiNotificationBinding>& notifications_;
    LifetimeToken lifetime_;
};
}
