#pragma once
#include "tui/app/tui_agent_attachment.hpp"
#include "tui/screen_port.hpp"
#include "utils/lifetime_token.hpp"
namespace acecode { struct TuiState; }
namespace acecode::tui {
class TuiOverlayGate : public TuiAgentAttachment {
public:
    TuiOverlayGate(TuiState& s, IScreenPort& scr, std::atomic<bool>& aborting)
        : state(s), screen(scr), agent_aborting(aborting) {}
    std::function<PermissionResult(const std::string&, const std::string&)> confirm_callback();
private:
    PermissionResult confirm(const std::string& tool_name, const std::string& args);
    TuiState& state;
    IScreenPort& screen;
    std::atomic<bool>& agent_aborting;
    LifetimeToken lifetime_;
};
}
