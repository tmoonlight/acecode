#pragma once
#include "tui/input/ports.hpp"
#include "tui/commands/command_registry.hpp"
#include "utils/lifetime_token.hpp"
namespace acecode::tui {
class TuiCommandContextFactory final : public ICommandContextFactory {
public:
    TuiCommandContextFactory(TuiState& state, AgentLoop& agent, SessionModelBinding& binding,
        AppConfig& config, TokenTracker& tracker, PermissionManager& permissions,
        IScreenPort& screen, SessionManager& session, McpManager& mcp, ToolExecutor& tools,
        SkillRegistry& skills, MemoryRegistry& memory, CommandRegistry& commands,
        const std::string& cwd, ITurnSubmitter& submitter,
        SubagentHost* subagents, const std::unique_ptr<IFullScreenSurfaces>& surfaces,
        std::function<void()> publish_config = {});
    CommandContext make(bool track_command_usage) override;
private:
    void record_usage(const std::string& name);
    TuiState& state_;
    AgentLoop& agent_;
    SessionModelBinding& binding_;
    AppConfig& config_;
    TokenTracker& tracker_;
    PermissionManager& permissions_;
    IScreenPort& screen_;
    SessionManager& session_;
    McpManager& mcp_;
    ToolExecutor& tools_;
    SkillRegistry& skills_;
    MemoryRegistry& memory_;
    CommandRegistry& commands_;
    const std::string& cwd_;
    ITurnSubmitter& submitter_;
    SubagentHost* subagents_;  // Nullable, borrowed; optional CommandContext service.
    // Borrow the app's fixed owner slot only during events, never construction.
    // The concrete surface is created after the chat/frame components.
    const std::unique_ptr<IFullScreenSurfaces>& surfaces_;
    std::function<void()> publish_config_;
    LifetimeToken lifetime_;
};
}
