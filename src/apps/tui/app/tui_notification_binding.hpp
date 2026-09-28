#pragma once
#include "tui/input/ports.hpp"
#include "utils/lifetime_token.hpp"
namespace acecode { struct AppConfig; struct TuiState; class SessionManager; }
namespace acecode::tui {
class ChatViewport;
class TuiNotificationBinding {
public:
    TuiNotificationBinding(const AppConfig& config, TuiState& state, IScreenPort& screen,
        ChatViewport& viewport, SessionManager& session, ICommandContextFactory& commands);
    ~TuiNotificationBinding();
    bool ready() const { return ready_; }
    void* window() const { return window_; }
    void shutdown();
private:
    void activate(const std::string& session_id);
    TuiState& state_;
    IScreenPort& screen_;
    ChatViewport& viewport_;
    SessionManager& session_;
    ICommandContextFactory& commands_;
    void* window_ = nullptr;  // Nullable, borrowed native terminal window.
    bool ready_ = false;
    bool stopped_ = false;
    LifetimeToken lifetime_;
};
}
