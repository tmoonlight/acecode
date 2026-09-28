#include "tui/app/full_screen_surfaces.hpp"
#include "tui/settings/settings_center.hpp"
#include "tui/settings/management_center.hpp"
#include "tui/tui_state.hpp"
#include "tui/subagent_host.hpp"
#include "tui/commands/command_registry.hpp"
#include "tool/mcp_scope.hpp"
#include "tool/mcp_manager.hpp"
#include "version.hpp"
#include <ftxui/component/component.hpp>
using ftxui::Event;
namespace acecode::tui {
FullScreenSurfaces::FullScreenSurfaces(TuiState& state, IScreenPort& screen, AppConfig& config,
    SessionManager& session, AgentLoop& agent, SubagentHost& subagents, SkillRegistry& skills,
    CommandRegistry& commands, McpManager& mcp, ToolExecutor& tools, HookManager& hooks,
    SkillUsageStore* skill_usage, const std::string& cwd, ftxui::Component chat, ftxui::Component input)
    : state_(state), screen_(screen), config_(config), session_(session), agent_(agent),
      subagents_(subagents), mcp_(mcp), tools_(tools), cwd_(cwd),
      active_surface_(static_cast<int>(settings::RootSurface::Chat)), input_(std::move(input)) {
    settings_ = std::make_unique<settings::SettingsCenter>(settings::SettingsCenterDependencies{
        &config_, cwd_, ACECODE_VERSION, {},
        bind(&FullScreenSurfaces::close),
        bind(&FullScreenSurfaces::post_event),
        bind(&FullScreenSurfaces::post_to_ui),
        bind(&FullScreenSurfaces::model_is_busy),
        bind(&FullScreenSurfaces::session_is_busy),
    });
    management_ = std::make_unique<settings::ManagementCenter>(settings::ManagementCenterDependencies{
        &config_, &skills, &commands, &mcp_, &tools_, &hooks, skill_usage, cwd_,
        bind(&FullScreenSurfaces::close),
        bind(&FullScreenSurfaces::post_event),
        bind(&FullScreenSurfaces::post_to_ui),
        bind(&FullScreenSurfaces::mcp_changed),
    });
    root_ = ftxui::Container::Tab({std::move(chat), settings_->component(), management_->component()},
        &active_surface_);
}
FullScreenSurfaces::~FullScreenSurfaces() { lifetime_.revoke(); }
void FullScreenSurfaces::close() {
    active_surface_ = static_cast<int>(settings::RootSurface::Chat);
    input_->TakeFocus();
    screen_.post_event(Event::Custom);
}
void FullScreenSurfaces::post_event() { screen_.post_event(Event::Custom); }
void FullScreenSurfaces::post_to_ui(std::function<void()> task) { screen_.post_task(std::move(task)); }
bool FullScreenSurfaces::model_is_busy(const std::string& model) {
    return subagents_.registry().model_profile_used_by_busy_session(model);
}
bool FullScreenSurfaces::session_is_busy(const std::string& session) {
    if (session_.current_session_id() == session) return agent_.is_busy();
    auto entry = subagents_.registry().acquire(session);
    return entry && entry->loop && entry->loop->is_busy();
}
void FullScreenSurfaces::mcp_changed() {
    mcp_.reconcile_scope("", config_.mcp_servers, tools_);
    agent_.set_tool_capability_policy(
        mcp_scope_policy(&config_, cwd_, std::nullopt, &mcp_, &tools_));
    subagents_.registry().refresh_mcp_policy(config_);
}
bool FullScreenSurfaces::foreground_surface_available(std::string& error) {
    std::lock_guard<std::mutex> lock(state_.mu);
    if (state_.ask_pending || state_.confirm_pending) {
        error =
            "Answer the active question or permission request before "
            "opening a full-screen settings view.";
        return false;
    }
    if (state_.is_waiting || state_.tool_running || state_.is_compacting) {
        error =
            "Settings can be opened when the foreground turn is idle. "
            "Background tasks may continue running.";
        return false;
    }
    return true;
}
bool FullScreenSurfaces::open_settings(const std::string& tab_slug, std::string& error) {
    if (!foreground_surface_available(error)) return false;
    if (tab_slug.empty()) {
        settings_->open();
    } else {
        auto tab =
            acecode::tui::settings::parse_settings_tab(tab_slug);
        if (!tab.has_value()) {
            error =
                "Unknown /config page '" + tab_slug +
                "'. Use general, appearance, configuration, "
                "personalization, models, usage, archived, or about.";
            return false;
        }
        settings_->open(*tab);
    }
    active_surface_ =
        static_cast<int>(
            acecode::tui::settings::RootSurface::Settings);
    settings_->component()->TakeFocus();
    screen_.post_event(Event::Custom);
    return true;
}
bool FullScreenSurfaces::open_management(const std::string& tab_slug, std::string& error) {
    if (!foreground_surface_available(error)) return false;
    auto tab =
        acecode::tui::settings::parse_management_tab(tab_slug);
    if (!tab.has_value()) {
        error =
            "Unknown capability page '" + tab_slug +
            "'. Use skills, mcp, connectors, tools, or hooks.";
        return false;
    }
    management_->open(*tab);
    active_surface_ =
        static_cast<int>(
            acecode::tui::settings::RootSurface::Management);
    management_->component()->TakeFocus();
    screen_.post_event(Event::Custom);
    return true;
}
}
