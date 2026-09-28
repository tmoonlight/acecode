#pragma once
#include "tui/input/ports.hpp"
#include "tui/app/tui_agent_attachment.hpp"
#include "provider/session_model_binding.hpp"
#include "utils/lifetime_token.hpp"
namespace acecode { struct TuiState; class McpManager; class AutoTitleRunner; }
namespace acecode::tui {
class TuiSubmitter final : public ITurnSubmitter, public TuiAgentAttachment {
public:
    TuiSubmitter(TuiState& state, IScreenPort& screen, AppConfig& config,
        SessionModelBinding& binding, SessionManager& session, McpManager& mcp,
        std::atomic<bool>& first_wait_done,
        const std::unique_ptr<AutoTitleRunner>& auto_title,
        std::function<void()> publish_config = {});
    std::string cwd() const override;
    void cancel() override;
    void submit_shell(const std::string& command) override;
    void before_first_turn() override;
    void submit_input(const UserInput& input) override;
    void submit_text(const std::string& text, const std::string& display_text = {}) override;
    std::function<void(const UserInput&)> callback();
private:
    SessionModelResolvedTarget resolve_model(const std::string& name);
    bool apply_transition(const SessionModelState& state, const SessionModelTransition& transition);
    TuiState& state_;
    IScreenPort& screen_;
    AppConfig& config_;
    SessionModelBinding& binding_;
    SessionManager& session_;
    McpManager& mcp_;
    std::atomic<bool>& first_wait_done_;
    // Fixed owner slot; populated at the original title startup step before events.
    const std::unique_ptr<AutoTitleRunner>& auto_title_;
    std::function<void()> publish_config_;
    LifetimeToken lifetime_;
};
}
