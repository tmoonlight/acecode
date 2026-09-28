#pragma once
#include "tui/input/ports.hpp"
#include "utils/lifetime_token.hpp"
#include <ftxui/component/component_base.hpp>
#include <memory>
#include <type_traits>
#include <utility>
namespace acecode {
struct TuiState; struct AppConfig; class SessionManager; class AgentLoop;
class SkillRegistry; class CommandRegistry; class McpManager; class ToolExecutor;
class HookManager; class SkillUsageStore;
}
namespace acecode::tui {
class SubagentHost;
namespace settings { class SettingsCenter; class ManagementCenter; }
class FullScreenSurfaces final : public IFullScreenSurfaces {
public:
    FullScreenSurfaces(TuiState& state, IScreenPort& screen, AppConfig& config,
        SessionManager& session, AgentLoop& agent, SubagentHost& subagents,
        SkillRegistry& skills, CommandRegistry& commands, McpManager& mcp,
        ToolExecutor& tools, HookManager& hooks, SkillUsageStore* skill_usage,
        const std::string& cwd, ftxui::Component chat, ftxui::Component input,
        std::function<void()> publish_config = {});
    ~FullScreenSurfaces();
    bool open_settings(const std::string& tab, std::string& error) override;
    bool open_management(const std::string& tab, std::string& error) override;
    ftxui::Component component() const { return root_; }
private:
    template<class R, class... Args>
    auto bind(R (FullScreenSurfaces::*method)(Args...)) {
        return [ref = lifetime_.ref(*this), method](Args... args) -> R {
            if constexpr (std::is_void_v<R>) {
                ref.with([&](FullScreenSurfaces& owner) { (owner.*method)(std::forward<Args>(args)...); });
            } else {
                R result{};
                ref.with([&](FullScreenSurfaces& owner) { result = (owner.*method)(std::forward<Args>(args)...); });
                return result;
            }
        };
    }
    void close();
    void post_event();
    void post_to_ui(std::function<void()> task);
    bool model_is_busy(const std::string& model);
    bool session_is_busy(const std::string& session);
    void mcp_changed();
    void skills_changed();
    bool foreground_surface_available(std::string& error);
    TuiState& state_;
    IScreenPort& screen_;
    AppConfig& config_;
    SessionManager& session_;
    AgentLoop& agent_;
    SubagentHost& subagents_;
    McpManager& mcp_;
    ToolExecutor& tools_;
    const std::string& cwd_;
    int active_surface_ = 0;
    // Shared with the component tree, retained for focus restoration on close.
    ftxui::Component input_;
    std::unique_ptr<settings::SettingsCenter> settings_;
    std::unique_ptr<settings::ManagementCenter> management_;
    ftxui::Component root_;  // Shared with the screen loop for the same UI lifetime.
    std::function<void()> publish_config_;
    LifetimeToken lifetime_;
};
}
