#pragma once
#include "agent/agent_loop.hpp"
#include "tui/tui_state.hpp"
#include "tui/screen_port.hpp"
#include "utils/joining_thread.hpp"
#include "utils/lifetime_token.hpp"
namespace acecode { class CopilotProvider; }
namespace acecode::tui {
class CopilotAuthTask {
public:
    CopilotAuthTask(const AgentLoop::ProviderAccessor& provider, TuiState& state,
        IScreenPort& screen, std::atomic<bool>& auth_done);
    void join() { worker_.join(); }
private:
    void authenticate();
    TuiState& state;
    IScreenPort& screen;
    std::atomic<bool>& auth_done;
    CopilotProvider* copilot = nullptr;  // Nullable, borrowed; O-07 replaces legacy borrowing.
    std::string copilot_model;
    LifetimeToken lifetime_;
    JoiningThread worker_;  // Joins before token/dependencies; owns the this capture.
};
}
